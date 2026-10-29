/**
 * @file TimelineSemaphore.h
 * @brief A Vulkan timeline semaphore: a 64-bit GPU counter the host can read.
 *
 * Binary semaphores only say "this submit finished"; they cannot say *which*
 * submit, and they cannot be waited on by value. A timeline semaphore is one
 * monotonic counter: every submit signals the next value, so "has frame N
 * completed?" becomes a single comparison against the counter instead of a
 * fence per frame - which is what lets an offscreen render thread publish its
 * progress without the main thread owning any of its fences.
 *
 * The device must be created with `timelineSemaphore = VK_TRUE` (core in
 * Vulkan 1.2, so no extension string is needed).
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

namespace neurus
{

/**
 * @brief Owns one timeline semaphore, created at value 0.
 *
 * Non-copyable and non-movable: the counter's identity is the whole point, and
 * a moved-from wrapper would leave two objects reading the same GPU object
 * through two different wrappers.
 */
class TimelineSemaphore
{
public:
	/**
	 * @brief Creates the semaphore with `vk::SemaphoreType::eTimeline`, value 0.
	 * @param device     Borrowed logical device; must have timeline semaphores
	 *                   enabled and must outlive this object.
	 * @param debugName  Optional object name, applied in Debug builds only.
	 */
	explicit TimelineSemaphore(const vk::raii::Device& device, const char* debugName = nullptr);

	TimelineSemaphore(const TimelineSemaphore&)            = delete;
	TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;
	TimelineSemaphore(TimelineSemaphore&&)                 = delete;
	TimelineSemaphore& operator=(TimelineSemaphore&&)      = delete;

	/**
	 * @brief Reads the counter without blocking.
	 *
	 * `vkGetSemaphoreCounterValue` is a host query, not a wait: it reports what
	 * the GPU has reached so far, so this is safe to call from any thread,
	 * including one that must not stall.
	 *
	 * @return The counter's current value.
	 */
	uint64_t Value() const;

	/**
	 * @brief Advances the counter from the host.
	 *
	 * This is the legal way to release a queue wait that has not been reached
	 * yet: a submit parked on a value nobody signalled is freed by advancing
	 * the counter to it. The value must be greater than the current value
	 * (VUID-VkSemaphoreSignalInfo-value-03258), less than the value of any
	 * pending queue signal on the same semaphore (VUID-VkSemaphoreSignalInfo-
	 * value-03259 - a pending *wait* is exactly what this releases, only
	 * pending signals constrain it), and within
	 * `maxTimelineSemaphoreValueDifference`.
	 *
	 * @param value  New counter value.
	 */
	void Signal(uint64_t value);

	/**
	 * @brief Blocks until the counter reaches @p value, or the timeout expires.
	 *
	 * @param value      Value to wait for.
	 * @param timeoutNs  Timeout in nanoseconds (`UINT64_MAX` waits indefinitely).
	 * @return True when the counter reached @p value, false on timeout.
	 *
	 * @throws std::runtime_error on any other non-success result (device loss
	 *         arrives as an exception from Vulkan-HPP before this check).
	 */
	bool Wait(uint64_t value, uint64_t timeoutNs) const;

	/** @brief The wrapped handle, for submissions that signal it. */
	vk::Semaphore Handle() const { return *m_semaphore; }

private:
	/// @brief Borrowed for every host-side operation; must outlive this object.
	const vk::raii::Device& m_device;
	vk::raii::Semaphore m_semaphore = nullptr;
};

} // namespace neurus
