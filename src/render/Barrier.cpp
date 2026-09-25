#include "Barrier.h"

namespace neurus {

// ---------------------------------------------------------------------------
// ToVulkanImageState
// ---------------------------------------------------------------------------

VulkanImageState Barrier::ToVulkanImageState(ImageState state)
{
	switch (state)
	{
	// Undefined/Invalid only ever appear as the *source* of a transition, i.e. a
	// discard: the old contents are not preserved, so no cache flush is needed and
	// the access mask stays empty. The stage is eAllCommands rather than eTopOfPipe
	// because eTopOfPipe in a srcStageMask creates no execution dependency at all —
	// a discard transition of an image someone else is still reading (a freshly
	// acquired swapchain image, read by the presentation engine) would be unordered
	// against that read. An execution-only dependency is enough to order it.
	case ImageState::Undefined:
		return {
			vk::ImageLayout::eUndefined,
			vk::PipelineStageFlagBits2::eAllCommands,
			vk::AccessFlagBits2::eNone
		};

	case ImageState::Invalid:
		return {
			vk::ImageLayout::eUndefined,
			vk::PipelineStageFlagBits2::eAllCommands,
			vk::AccessFlagBits2::eNone
		};

	case ImageState::TransferSrc:
		return {
			vk::ImageLayout::eTransferSrcOptimal,
			vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eTransferRead
		};

	case ImageState::TransferDst:
		return {
			vk::ImageLayout::eTransferDstOptimal,
			vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eTransferWrite
		};

	// The read bits are not optional: an attachment reached with
	// VK_ATTACHMENT_LOAD_OP_LOAD (DebugPass draws over ComposedOutput) or read by
	// the depth test without depth writes (DebugPass again) *reads* the image
	// through the attachment stage, so a write-only dstAccessMask leaves the
	// preceding writer's data unavailable to the load — on a tiler that surfaces
	// as stale tiles blended into the frame. Passes that only clear-and-write are
	// unaffected by the wider mask.
	case ImageState::ColorAttachment:
		return {
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::AccessFlagBits2::eColorAttachmentWrite |
			    vk::AccessFlagBits2::eColorAttachmentRead
		};

	case ImageState::DepthAttachment:
		return {
			vk::ImageLayout::eDepthStencilAttachmentOptimal,
			vk::PipelineStageFlagBits2::eEarlyFragmentTests |
			    vk::PipelineStageFlagBits2::eLateFragmentTests,
			vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
			    vk::AccessFlagBits2::eDepthStencilAttachmentRead
		};

	case ImageState::ColorShaderRead:
		return {
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::PipelineStageFlagBits2::eFragmentShader |
			    vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderRead
		};

	case ImageState::DepthShaderRead:
		return {
			vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			vk::PipelineStageFlagBits2::eFragmentShader |
			    vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderRead
		};

	case ImageState::ShaderWrite:
		return {
			vk::ImageLayout::eGeneral,
			vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderWrite
		};

	case ImageState::Present:
		return {
			vk::ImageLayout::ePresentSrcKHR,
			vk::PipelineStageFlagBits2::eBottomOfPipe,
			vk::AccessFlagBits2::eNone
		};
	}

	// Fallback (should never reach here)
	return {
		vk::ImageLayout::eUndefined,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::AccessFlagBits2::eNone
	};
}

// ---------------------------------------------------------------------------
// Transition (full image)
// ---------------------------------------------------------------------------

void Barrier::Transition(VkCommandBuffer cmd,
                          Image& image,
                          ImageState after)
{
	Transition(cmd, image, after, image.AllSubresources());
	image.im_state = after;
}

// ---------------------------------------------------------------------------
// Transition (explicit subresource range — does NOT update im_state)
// ---------------------------------------------------------------------------

void Barrier::Transition(VkCommandBuffer cmd,
                          Image& image,
                          ImageState after,
                          const vk::ImageSubresourceRange& subresourceRange)
{
	const ImageState before = image.im_state;

	const auto beforeState = ToVulkanImageState(before);
	const auto afterState  = ToVulkanImageState(after);

	const vk::ImageMemoryBarrier2 barrier(
		beforeState.stage,
		beforeState.access,
		afterState.stage,
		afterState.access,
		beforeState.layout,
		afterState.layout,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		*image.ImageHandle(),
		subresourceRange);

	const vk::DependencyInfo depInfo({}, {}, {}, barrier);
	vk::CommandBuffer(cmd).pipelineBarrier2(depInfo);
}

// ---------------------------------------------------------------------------
// Transition (raw vk::Image with explicit before/after — no state tracking)
// ---------------------------------------------------------------------------

void Barrier::Transition(VkCommandBuffer cmd,
                         vk::Image image,
                         ImageState before,
                         ImageState after,
                         const vk::ImageSubresourceRange& subresourceRange)
{
	const auto beforeState = ToVulkanImageState(before);
	const auto afterState  = ToVulkanImageState(after);

	const vk::ImageMemoryBarrier2 barrier(
		beforeState.stage,
		beforeState.access,
		afterState.stage,
		afterState.access,
		beforeState.layout,
		afterState.layout,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		image,
		subresourceRange);

	const vk::DependencyInfo depInfo({}, {}, {}, barrier);
	vk::CommandBuffer(cmd).pipelineBarrier2(depInfo);
}

// ---------------------------------------------------------------------------
// ReleaseToRead (transfer-queue upload release — no graphics dstStage)
// ---------------------------------------------------------------------------

void Barrier::ReleaseToRead(VkCommandBuffer cmd,
                            vk::Image image,
                            ImageState before,
                            const vk::ImageSubresourceRange& subresourceRange)
{
	const auto beforeState = ToVulkanImageState(before);

	// Make transfer writes available to subsequent graphics usage WITHOUT a
	// graphics-stage dstStageMask, which is invalid on transfer-only command
	// pools (VUID-vkCmdPipelineBarrier2-dstStageMask-09676). The layout still
	// becomes ShaderReadOnlyOptimal so the graphics side can sample the image;
	// the consuming pass re-transitions it on first use.
	const vk::ImageMemoryBarrier2 barrier(
		beforeState.stage,
		beforeState.access,
		vk::PipelineStageFlagBits2::eBottomOfPipe,
		vk::AccessFlagBits2::eNone,
		beforeState.layout,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		image,
		subresourceRange);

	const vk::DependencyInfo depInfo({}, {}, {}, barrier);
	vk::CommandBuffer(cmd).pipelineBarrier2(depInfo);
}

// ---------------------------------------------------------------------------
// Buffer state → Vulkan mapping
// ---------------------------------------------------------------------------

namespace {

struct VulkanBufferState
{
	vk::PipelineStageFlags2 stage;
	vk::AccessFlags2 access;
};

VulkanBufferState ToVulkanBufferState(BufferState state)
{
	switch (state)
	{
	case BufferState::Undefined:
		return { vk::PipelineStageFlagBits2::eTopOfPipe,
		         vk::AccessFlagBits2::eNone };

	case BufferState::HostWrite:
		return { vk::PipelineStageFlagBits2::eHost,
		         vk::AccessFlagBits2::eHostWrite };

	case BufferState::HostRead:
		return { vk::PipelineStageFlagBits2::eHost,
		         vk::AccessFlagBits2::eHostRead };

	case BufferState::TransferSrc:
		return { vk::PipelineStageFlagBits2::eTransfer,
		         vk::AccessFlagBits2::eTransferRead };

	case BufferState::TransferDst:
		return { vk::PipelineStageFlagBits2::eTransfer,
		         vk::AccessFlagBits2::eTransferWrite };

	case BufferState::ShaderRead:
		return { vk::PipelineStageFlagBits2::eVertexShader |
		         vk::PipelineStageFlagBits2::eFragmentShader |
		         vk::PipelineStageFlagBits2::eComputeShader,
		         vk::AccessFlagBits2::eShaderRead };

	case BufferState::ShaderWrite:
		return { vk::PipelineStageFlagBits2::eComputeShader,
		         vk::AccessFlagBits2::eShaderWrite };

	case BufferState::General:
		return { vk::PipelineStageFlagBits2::eAllCommands,
		         vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite };
	}

	return { vk::PipelineStageFlagBits2::eTopOfPipe,
	         vk::AccessFlagBits2::eNone };
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Buffer transition (state-tracked)
// ---------------------------------------------------------------------------

void Barrier::Transition(VkCommandBuffer cmd,
                         Buffer& buffer,
                         BufferState after)
{
	const BufferState before = buffer.b_state;

	const auto beforeState = ToVulkanBufferState(before);
	const auto afterState  = ToVulkanBufferState(after);

	const vk::BufferMemoryBarrier2 barrier(
		beforeState.stage, beforeState.access,
		afterState.stage, afterState.access,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		buffer.buffer(),
		0, VK_WHOLE_SIZE);

	const vk::DependencyInfo depInfo({}, {}, barrier, {});
	vk::CommandBuffer(cmd).pipelineBarrier2(depInfo);

	buffer.b_state = after;
}

} // namespace neurus
