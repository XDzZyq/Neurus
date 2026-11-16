#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <optional>

namespace neurus {

/**
 * @brief The image chain a frame is drawn through.
 *
 * A window presents through a swapchain; a headless renderer draws into its
 * attachments and presents nothing. The renderer is identical in both cases:
 * only the acquire/present handshake and the final blit depend on this seam.
 *
 * Call order the renderer uses: Acquire() once per frame, Image(index) while
 * recording, Present() after the submit, Recreate() on resize.
 */
class OutputTarget
{
public:
	virtual ~OutputTarget() = default;

	/** @brief Current target extent in pixels. */
	virtual vk::Extent2D Extent() const = 0;

	/** @brief Format of the image a frame is presented into. */
	virtual vk::Format Format() const = 0;

	/** @brief Number of images a frame may be recorded into (command buffers). */
	virtual uint32_t ImageCount() const = 0;

	/** @brief Handle of image @p index; null when the target presents nothing. */
	virtual vk::Image Image(uint32_t index) const = 0;

	/**
	 * @brief True when the target acquires and presents through a surface.
	 *
	 * False means: no acquire semaphore, no present, no final blit. The last
	 * image a frame produced is an attachment, not a presented image.
	 */
	virtual bool Presents() const = 0;

	/** @brief Monotonic generation; changes when the target rebuilt itself. */
	virtual uint32_t Generation() const = 0;

	/**
	 * @brief Acquires the image this frame records into.
	 * @param imageAvailable Semaphore signalled when the image is ready; may be
	 *                       null, and is ignored by a target that does not
	 *                       acquire.
	 * @return The image index, or std::nullopt when nothing was acquired
	 *         (out of date, minimized, surface lost). No semaphore was signalled
	 *         in that case, so the caller must not submit.
	 */
	virtual std::optional<uint32_t> Acquire(const vk::raii::Semaphore* imageAvailable) = 0;

	/** @brief Presents image @p index; a no-op when the target presents nothing. */
	virtual void Present(const vk::raii::Semaphore& renderFinished, uint32_t index) = 0;

	/**
	 * @brief Resizes the target.
	 *
	 * Must be a no-op (and must not bump Generation()) when the size did not
	 * change: the caller reallocates every screen-space attachment on a
	 * generation change.
	 */
	virtual void Recreate(uint32_t width, uint32_t height) = 0;
};

} // namespace neurus
