/**
 * @file DescriptorBinder.h
 * @brief Writes a pass's declared image attachments into its descriptor set.
 *
 * DescriptorBinder turns a pass's `PassIO` declaration into concrete descriptor
 * writes. Each `AttachmentBinding` carries the shader binding slot, the
 * descriptor type (combined image sampler vs storage image), and the image
 * layout the shader expects. A single shared sampler is supplied by the caller
 * and used for every combined-image-sampler binding; storage-image bindings
 * ignore it.
 *
 * @note Performs no barriers and no layout transitions. The caller must
 *       transition attachments to `imageLayout` before dispatch.
 */

#pragma once

#include "../RenderCache.h"
#include "../Image.h"
#include "../DescriptorManager.h"
#include "../passes/Pass.h"

#include <vulkan/vulkan_raii.hpp>

#include <span>

namespace neurus {

/**
 * @brief Stateless helper that materializes image descriptors from PassIO.
 */
struct DescriptorBinder
{
	/**
	 * @brief Writes one image descriptor per binding in @p reads and @p writes.
	 *
	 * @param dst      Descriptor set to write into (one per in-flight frame).
	 * @param reads    Sampled/input attachments declared by the pass.
	 * @param writes   Storage/output attachments declared by the pass.
	 * @param cache    Resolves each AttachmentName to its GPU Image view.
	 * @param extent   Render extent used for attachment lookup.
	 * @param sampler  Shared sampler bound to combined-image-sampler slots;
	 *                 unused (may be null) for storage-image slots.
	 */
	static void BindImages(DescriptorSet& dst,
	                       std::span<const AttachmentBinding> reads,
	                       std::span<const AttachmentBinding> writes,
	                       RenderCache& cache,
	                       vk::Extent2D extent,
	                       vk::Sampler sampler)
	{
		auto write = [&](const AttachmentBinding& b)
		{
			const auto& att = cache.GetAttachment(b.resource, extent);

			// Storage images are bound without a sampler; sampled images use
			// the shared nearest-neighbour sampler supplied by the caller.
			const vk::Sampler boundSampler =
				(b.descriptorType == vk::DescriptorType::eCombinedImageSampler)
					? sampler
					: vk::Sampler{};

			vk::DescriptorImageInfo imageInfo(
				boundSampler,
				*att.ImageViewHandle(),
				b.imageLayout);

			dst.WriteImage(b.binding, imageInfo, b.descriptorType);
		};

		for (const auto& b : reads)  write(b);
		for (const auto& b : writes) write(b);
	}
};

} // namespace neurus
