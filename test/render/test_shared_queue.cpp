/**
 * @file test_shared_queue.cpp
 * @brief SharedQueue must make one queue handle safe to submit from many
 *        threads, without turning a GPU wait into a lock every thread shares.
 *
 * Two properties are pinned here, and both are easy to fake:
 *
 *   1. **A submit is complete when the caller says so.** SubmitAndWait() and
 *      WaitIdle() are only useful if the work they wait for has actually landed
 *      in memory the host can read. Every test therefore reads the filled value
 *      back and compares it - a wrapper that skipped the wait, or forgot the
 *      transfer-to-host barrier, fails on the data rather than on a timing
 *      assumption.
 *   2. **Submit, wait and readback hold together under concurrency.** Four
 *      workers submit their own command buffers through one queue, each into
 *      its own slot; every slot must end holding its worker's last value. Slots
 *      are per worker on purpose - a shared cell would let a lost or reordered
 *      submission still "pass".
 *   3. **The lock covers the call, not the GPU work.** One thread's
 *      SubmitAndWait() parked on a timeline value must not stop another thread
 *      from submitting: if the interlock were held across the fence wait, a
 *      long bake would freeze every other submitter. The main thread is the only
 *      signaller and never takes the queue lock, so a wrapper that got this
 *      wrong shows up as a late future instead of as a hung test.
 *
 * The workers are the real client shape: command pools are externally
 * synchronised too, so each worker owns one, and the queue is the only thing
 * shared. No sleeps and no timing windows: the main thread joins the workers and
 * then inspects the result, so a missing wait shows up as wrong data instead of
 * as a flaky failure.
 *
 * @note Requires a Vulkan 1.4-capable GPU; skipped without one.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/SharedQueue.h"
#include "render/TimelineSemaphore.h"
#include "render/buffers/CPUBuffer.h"

#include "core/Log.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace neurus;

namespace
{

/// @brief Bytes per slot: exactly one fillBuffer value.
constexpr vk::DeviceSize kSlotBytes = 4;

/// @brief Generous upper bound for a single fence wait; never a timing assumption.
constexpr uint64_t kFenceTimeoutNs = 1'000'000'000ULL;

/**
 * @brief Records one fillBuffer into a reusable command buffer.
 *
 * The command buffer is reset first so a caller can record the same allocation
 * every iteration; resetting is legal in the initial state as well, so the first
 * use needs no special case.
 *
 * The barrier is what makes the readback honest: the fill is a transfer write,
 * and a host read afterwards needs it made available and visible to the host
 * domain. Without it the fence can be signalled while the host still observes
 * stale memory.
 *
 * @param cmd     Command buffer to record into; must not be pending.
 * @param buffer  Destination buffer, created with eTransferDst usage.
 * @param offset  Byte offset of the slot within @p buffer.
 * @param value   The 32-bit value to fill the slot with.
 */
void RecordFill(const vk::raii::CommandBuffer& cmd,
	vk::Buffer buffer,
	vk::DeviceSize offset,
	uint32_t value)
{
	cmd.reset();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	cmd.fillBuffer(buffer, offset, kSlotBytes, value);

	const vk::BufferMemoryBarrier2 toHost(
		vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eHost,
		vk::AccessFlagBits2::eHostRead,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		buffer,
		offset,
		kSlotBytes);
	cmd.pipelineBarrier2(vk::DependencyInfo({}, {}, toHost));

	cmd.end();
}

/**
 * @brief Reads one 4-byte slot out of a host-mapped buffer.
 *
 * Copied out rather than reinterpret_cast in place: the mapping's alignment is
 * the driver's business, and the point of the test is the value, not the pointer.
 *
 * @param buffer  Host-visible buffer to read.
 * @param offset  Byte offset of the slot.
 * @return The stored 32-bit value.
 */
uint32_t ReadSlot(const CPUBuffer& buffer, vk::DeviceSize offset)
{
	uint32_t value = 0;
	std::memcpy(&value, static_cast<const uint8_t*>(buffer.Data()) + offset, sizeof(value));
	return value;
}

/**
 * @brief Releases the timeline blocker and joins both threads, whatever happens.
 *
 * Thread A can only leave its wait once the blocker is signalled, so this is the
 * one thing that must run on every path - including the paths a test failure
 * takes. Armed before the threads exist and holding them by reference, it skips
 * whatever is already finished: the joinable() checks make it correct both on
 * the normal path (already joined) and on the exception path, where a joinable
 * std::thread would otherwise be destroyed (std::terminate) and A abandoned on a
 * value nobody would ever signal.
 */
struct BlockerGuard
{
	TimelineSemaphore& blocker;
	std::thread&       a;
	std::thread&       b;
	uint64_t           value;
	bool               released = false;

	/**
	 * @brief Signals the blocker once; called explicitly on the normal path.
	 *
	 * Idempotent because signalling the same value twice is a validation error,
	 * not a no-op (the value must strictly increase).
	 */
	void Release()
	{
		if (released)
		{
			return;
		}
		blocker.Signal(value);
		released = true;
	}

	~BlockerGuard() noexcept
	{
		try
		{
			Release();
		}
		// A destructor cannot propagate, but the joins below still have to run.
		catch (const std::exception& e)
		{
			NEURUS_ERR("[SharedQueueTest] Releasing the timeline blocker failed: " << e.what());
		}

		if (a.joinable())
		{
			a.join();
		}
		if (b.joinable())
		{
			b.join();
		}
	}
};

} // namespace

/**
 * @brief Fixture for SharedQueue tests; supplies the device and graphics queue.
 */
class SharedQueueTest : public VulkanTestShared
{
};

TEST_F(SharedQueueTest, SubmitAndWait_WorkIsCompleteOnReturn)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// The fixture's wrapper is the queue under test: one interlock, as in production.
	auto& queue = *m_queue;
	CPUBuffer slot(*m_device, PhysicalDevice(), kSlotBytes,
	               vk::BufferUsageFlagBits::eTransferDst, "SharedQueueTest_Slot");

	vk::raii::CommandPool pool(*m_device, vk::CommandPoolCreateInfo(
		vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_graphicsQueueFamily));
	vk::raii::CommandBuffers cmds(*m_device, vk::CommandBufferAllocateInfo(
		*pool, vk::CommandBufferLevel::ePrimary, 1));

	RecordFill(cmds[0], slot.buffer(), 0, 0xC0FFEE11u);

	ASSERT_NO_THROW(queue.SubmitAndWait(vk::SubmitInfo({}, {}, *cmds[0])));

	// No further wait: returning means the fill is already readable.
	EXPECT_EQ(ReadSlot(slot, 0), 0xC0FFEE11u);
}

TEST_F(SharedQueueTest, Submit_SignalsGivenFence)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// The fixture's wrapper is the queue under test: one interlock, as in production.
	auto& queue = *m_queue;
	CPUBuffer slot(*m_device, PhysicalDevice(), kSlotBytes,
	               vk::BufferUsageFlagBits::eTransferDst, "SharedQueueTest_Slot");

	vk::raii::CommandPool pool(*m_device, vk::CommandPoolCreateInfo(
		vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_graphicsQueueFamily));
	vk::raii::CommandBuffers cmds(*m_device, vk::CommandBufferAllocateInfo(
		*pool, vk::CommandBufferLevel::ePrimary, 1));

	RecordFill(cmds[0], slot.buffer(), 0, 0xC0FFEE11u);

	// The caller's fence, not a wrapper-owned one: Submit must hand the fence
	// straight to the submission so the caller can wait on its own terms.
	vk::raii::Fence fence(*m_device, vk::FenceCreateInfo{});
	queue.Submit(vk::SubmitInfo({}, {}, *cmds[0]), *fence);

	ASSERT_EQ(m_device->waitForFences(*fence, VK_TRUE, kFenceTimeoutNs), vk::Result::eSuccess);
	EXPECT_EQ(ReadSlot(slot, 0), 0xC0FFEE11u);
}

TEST_F(SharedQueueTest, WaitIdle_AfterSubmit_WorkComplete)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// The fixture's wrapper is the queue under test: one interlock, as in production.
	auto& queue = *m_queue;
	CPUBuffer slot(*m_device, PhysicalDevice(), kSlotBytes,
	               vk::BufferUsageFlagBits::eTransferDst, "SharedQueueTest_Slot");

	vk::raii::CommandPool pool(*m_device, vk::CommandPoolCreateInfo(
		vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_graphicsQueueFamily));
	vk::raii::CommandBuffers cmds(*m_device, vk::CommandBufferAllocateInfo(
		*pool, vk::CommandBufferLevel::ePrimary, 1));

	RecordFill(cmds[0], slot.buffer(), 0, 0xC0FFEE11u);

	// No fence at all: the drain itself is the only completion signal the caller
	// has, so the value must be readable once it returns.
	queue.Submit(vk::SubmitInfo({}, {}, *cmds[0]));
	ASSERT_NO_THROW(queue.WaitIdle());

	EXPECT_EQ(ReadSlot(slot, 0), 0xC0FFEE11u);
}

TEST_F(SharedQueueTest, ConcurrentSubmitAndWait_FourThreads_AllComplete)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr uint32_t kThreads = 4;
	constexpr uint32_t kIterations = 50;

	CPUBuffer slots(*m_device, PhysicalDevice(), kSlotBytes * kThreads,
	                vk::BufferUsageFlagBits::eTransferDst, "SharedQueueTest_Slots");
	// The fixture's wrapper is the queue under test: one interlock, as in production.
	auto& queue = *m_queue;

	// One entry per worker, written only by that worker: distinct elements of
	// the array, so no synchronisation is needed to collect the outcome. An
	// empty string means the worker finished without throwing.
	std::array<std::string, kThreads> failures{};

	std::vector<std::thread> workers;
	workers.reserve(kThreads);
	for (uint32_t t = 0; t < kThreads; ++t)
	{
		workers.emplace_back([&, t]
		{
			try
			{
				// Command pools are externally synchronised as well, so each
				// worker owns one; only the queue is deliberately shared.
				vk::raii::CommandPool pool(*m_device, vk::CommandPoolCreateInfo(
					vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_graphicsQueueFamily));
				vk::raii::CommandBuffers cmds(*m_device, vk::CommandBufferAllocateInfo(
					*pool, vk::CommandBufferLevel::ePrimary, 1));

				for (uint32_t i = 0; i < kIterations; ++i)
				{
					RecordFill(cmds[0], slots.buffer(), t * kSlotBytes, t * 1000u + i);
					queue.SubmitAndWait(vk::SubmitInfo({}, {}, *cmds[0]));
				}
			}
			// Reported after the join: an assertion here would abort only this
			// worker and leave the others running unattended.
			catch (const std::exception& e)
			{
				failures[t] = e.what();
			}
			catch (...)
			{
				failures[t] = "unknown exception";
			}
		});
	}

	for (auto& worker : workers)
	{
		worker.join();
	}

	for (uint32_t t = 0; t < kThreads; ++t)
	{
		EXPECT_TRUE(failures[t].empty()) << "worker " << t << " threw: " << failures[t];
		// The last iteration's value: anything older means a submit or its wait
		// was lost, anything from another worker means the slots overlapped.
		EXPECT_EQ(ReadSlot(slots, t * kSlotBytes), t * 1000u + (kIterations - 1))
			<< "worker " << t << " did not finish its last fill";
	}
}

TEST_F(SharedQueueTest, SubmitAndWait_ReleasesLockWhileWaiting)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// Thread A parks a submit on a timeline value only this thread can signal, so
	// its SubmitAndWait is genuinely waiting when thread B submits. The wrapper's
	// own fence wait is what A is inside of; whether the lock is held across it is
	// exactly what B's timing answers.
	TimelineSemaphore blocker(*m_device, "SharedQueueTest_Blocker");
	constexpr uint64_t kBlockingValue = 1;

	auto& queue = *m_queue;

	std::promise<void> aDone;
	std::future<void> aReturned = aDone.get_future();
	std::string aFailure;

	std::promise<void> bDone;
	std::future<void> bReturned = bDone.get_future();
	std::string bFailure;

	// Created and waited by this thread: B only hands the handle to its submit.
	vk::raii::Fence bFence(*m_device, vk::FenceCreateInfo{});

	std::thread a;
	std::thread b;

	// Armed before either thread exists, so every throw from here on still ends
	// with the blocker signalled and both threads joined - never with a joinable
	// std::thread destroyed (std::terminate) and A left parked on a value nobody
	// signals.
	BlockerGuard guard{blocker, a, b, kBlockingValue};

	a = std::thread([&]
	{
		try
		{
			vk::raii::CommandPool pool(*m_device, vk::CommandPoolCreateInfo(
				vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_graphicsQueueFamily));
			vk::raii::CommandBuffers cmds(*m_device, vk::CommandBufferAllocateInfo(
				*pool, vk::CommandBufferLevel::ePrimary, 1));

			cmds[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
			cmds[0].end();

			const vk::CommandBuffer cmdHandle = *cmds[0];
			const vk::Semaphore waitSemaphore = blocker.Handle();
			const vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
			const uint64_t waitValues[] = { kBlockingValue };
			const vk::TimelineSemaphoreSubmitInfo timelineInfo(1, waitValues, 0, nullptr);
			const vk::SubmitInfo submitInfo(1, &waitSemaphore, &waitStage, 1, &cmdHandle, 0, nullptr,
			                                &timelineInfo);

			queue.SubmitAndWait(submitInfo);
		}
		// Reported after the join: an assertion here would abort only this thread.
		catch (const std::exception& e)
		{
			aFailure = e.what();
		}
		catch (...)
		{
			aFailure = "unknown exception";
		}
		aDone.set_value();
	});

	// 50 ms so A is inside its wait, not merely on its way there. Nothing below
	// depends on the window: if A had not submitted yet, B would just be faster.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	b = std::thread([&]
	{
		try
		{
			vk::raii::CommandPool pool(*m_device, vk::CommandPoolCreateInfo(
				vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_graphicsQueueFamily));
			vk::raii::CommandBuffers cmds(*m_device, vk::CommandBufferAllocateInfo(
				*pool, vk::CommandBufferLevel::ePrimary, 1));

			// An empty submit: the point is that it reaches the queue at all, not
			// that it renders anything.
			cmds[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
			cmds[0].end();

			const vk::CommandBuffer cmdHandle = *cmds[0];
			queue.Submit(vk::SubmitInfo({}, {}, cmdHandle), *bFence);
		}
		catch (const std::exception& e)
		{
			bFailure = e.what();
		}
		catch (...)
		{
			bFailure = "unknown exception";
		}
		bDone.set_value();
	});

	// A future with a deadline, never the queue: this thread must not be able to
	// block on the very lock the test is about, or a wrong wrapper would hang the
	// suite instead of failing it.
	const std::future_status bStatus = bReturned.wait_for(std::chrono::seconds(1));

	// The only signaller, and unconditional: A's wait can only end here, so this
	// runs before any join no matter what B did - even when the wrapper is broken
	// and B is still stuck on the lock. The guard repeats it only if a throw
	// skipped this line.
	guard.Release();

	a.join();
	b.join();

	EXPECT_TRUE(aFailure.empty()) << "SubmitAndWait threw: " << aFailure;
	EXPECT_TRUE(bFailure.empty()) << "Submit threw: " << bFailure;
	EXPECT_EQ(bStatus, std::future_status::ready)
		<< "Submit was still blocked after 1 s while another thread's SubmitAndWait was "
		   "waiting its fence: the queue lock is held across the GPU wait";
	EXPECT_EQ(aReturned.wait_for(std::chrono::seconds(0)), std::future_status::ready)
		<< "SubmitAndWait did not return after the timeline value it waited for was signalled";

	// B's submit must really have landed: a fence that never signals would mean
	// the call returned without submitting anything.
	ASSERT_EQ(m_device->waitForFences(*bFence, VK_TRUE, kFenceTimeoutNs), vk::Result::eSuccess);
}
