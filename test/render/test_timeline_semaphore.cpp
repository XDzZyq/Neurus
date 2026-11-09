/**
 * @file test_timeline_semaphore.cpp
 * @brief TimelineSemaphore must report the GPU's counter as it really is.
 *
 * A timeline semaphore is only useful if three separate claims hold, and each
 * is pinned here against the API rather than against timing:
 *
 *   1. **It starts at 0 and the host can advance it.** Signal() is the only
 *      way a test can drive the counter without a GPU; if the initial value or
 *      the host advance were wrong, every later wait would be off by a
 *      constant.
 *   2. **A wait for an unreached value returns false, not a hang.** The timeout
 *      is the contract a poller relies on - the point is that Wait() gives up
 *      and says so, so the test uses a 1 ms deadline against a value nothing
 *      signals.
 *   3. **A GPU submit really moves the counter.** A host-only implementation
 *      (Signal() writing a local) would pass the first two tests; signalling 7
 *      from an actual submission and observing it through both Wait() and
 *      Value() is what proves the value comes from the GPU.
 *
 * @note Requires a Vulkan 1.4-capable GPU with timeline semaphores (core in
 *       Vulkan 1.2); skipped without one.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/TimelineSemaphore.h"

#include <cstdint>

using namespace neurus;

namespace
{

/// @brief A deadline for a wait that must succeed; never a timing assumption.
constexpr uint64_t kWaitTimeoutNs = 1'000'000'000ULL;

/// @brief A deadline for a wait that must fail: 1 ms, short enough that a
///        broken (blocking) implementation shows up as a hung test, not a slow
///        one.
constexpr uint64_t kShortTimeoutNs = 1'000'000ULL;

/// @brief The value the GPU submission in GpuSignal_ReachesValue signals.
constexpr uint64_t kGpuSignalValue = 7;

} // namespace

/**
 * @brief Fixture for TimelineSemaphore tests; supplies the device and queue.
 */
class TimelineSemaphoreTest : public VulkanTestShared
{
};

TEST_F(TimelineSemaphoreTest, Value_StartsAtZero)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	const TimelineSemaphore timeline(*m_device, "TimelineSemaphoreTest_StartsAtZero");

	// A timeline semaphore is created at the value its create info names: 0, so
	// "no frame completed yet" is a value, not a null handle to special-case.
	EXPECT_EQ(timeline.Value(), 0ULL);
}

TEST_F(TimelineSemaphoreTest, HostSignal_ValueAdvances)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	TimelineSemaphore timeline(*m_device, "TimelineSemaphoreTest_HostSignal");

	timeline.Signal(3);
	EXPECT_EQ(timeline.Value(), 3ULL);

	// Non-consecutive jumps are legal and must land exactly: a caller may skip
	// values it never used.
	timeline.Signal(9);
	EXPECT_EQ(timeline.Value(), 9ULL);
}

TEST_F(TimelineSemaphoreTest, Wait_TimesOutBelowValue)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	TimelineSemaphore timeline(*m_device, "TimelineSemaphoreTest_Timeout");

	// Nothing signals this semaphore: the wait has to expire and report false
	// rather than block forever or throw for a 1 ms timeout.
	EXPECT_FALSE(timeline.Wait(1, kShortTimeoutNs));

	// A timed-out wait must leave the counter alone - a poller that timed out
	// never "consumed" the value it asked for.
	EXPECT_EQ(timeline.Value(), 0ULL);
}

TEST_F(TimelineSemaphoreTest, GpuSignal_ReachesValue)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	TimelineSemaphore timeline(*m_device, "TimelineSemaphoreTest_GpuSignal");

	// An empty submission is enough: the signal is the whole payload, so the
	// value can only come from the queue, never from the recording.
	auto& cmd = m_commandBuffers[0];
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	cmd.end();

	const vk::CommandBuffer cmdHandle = *cmd;
	const vk::Semaphore signalSemaphore = timeline.Handle();
	const uint64_t signalValues[] = { kGpuSignalValue };
	const vk::TimelineSemaphoreSubmitInfo timelineInfo(0, nullptr, 1, signalValues);
	const vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmdHandle, 1, &signalSemaphore,
	                                &timelineInfo);

	// Waits its own fence, so the submission has completed when this returns -
	// the semaphore is then either signalled or the submit lied.
	m_queue->SubmitAndWait(submitInfo);

	EXPECT_TRUE(timeline.Wait(kGpuSignalValue, kWaitTimeoutNs))
		<< "submission signalled " << kGpuSignalValue << " but the wait did not observe it";
	EXPECT_EQ(timeline.Value(), kGpuSignalValue);
}
