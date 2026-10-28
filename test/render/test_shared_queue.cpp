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
#include "render/buffers/CPUBuffer.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
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
