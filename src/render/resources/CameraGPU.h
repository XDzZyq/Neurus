/**
 * @file CameraGPU.h
 * @brief The renderer's single camera uniform buffer, shared by every pass.
 *
 * Every pass that transforms geometry needs the same {viewProj, view} block, and
 * before this existed each one owned a private UniformBuffer and uploaded a
 * byte-identical copy of it per frame — GeometryPass and DebugPass computed the
 * same two matrices from the same Camera and memcpy'd them into two different
 * buffers. CameraGPU makes the camera a RenderCache resource like the light
 * SSBOs: one writer per frame (DeferredRenderer::recordFrame, through
 * RenderCache::UpdateCamera), and passes only bind and read.
 */

#pragma once

#include "../buffers/UniformBuffer.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace neurus {

/**
 * @brief Camera data uploaded to the GPU each frame (128 B).
 *
 * Contains the combined view-projection matrix and the view matrix (needed for
 * view-space normal computation in the vertex shader). Mirrors the `CameraUBO`
 * block in gbuffer.vert, debug_line.vert, debug_point.vert and debug_wire.vert —
 * all four declare it identically, which is what lets them share one buffer.
 */
struct CameraUBOData
{
	glm::mat4 viewProj;   ///< projection * view
	glm::mat4 view;       ///< view matrix (for normal transform)
};

static_assert(sizeof(CameraUBOData) == 128, "must match the GLSL CameraUBO block");

/**
 * @brief Owns the shared camera UBO and the matrices currently in it.
 *
 * One buffer, not a ring: the renderer runs with `kMaxFramesInFlight == 1`
 * (RenderCache::GetAttachment hands out a single Image per AttachmentName, so a
 * second in-flight frame would already share its G-Buffer), and each frame waits
 * on its fence before recording, so the CPU never overwrites a UBO the GPU is
 * still reading. That also keeps every consumer's descriptor write a one-time
 * cost: the VkBuffer handle never changes. If frames in flight ever grow beyond
 * one, this becomes a ring indexed by frame and every consumer needs one
 * descriptor set per frame — the same change DebugCache already carries.
 *
 * Non-copyable (owns a GPU buffer), movable.
 */
class CameraGPU
{
public:
	/**
	 * @brief Allocates the uniform buffer; contents are undefined until Update().
	 *
	 * @param device          Logical device (borrowed, must outlive this object).
	 * @param physicalDevice  Physical device for memory type queries.
	 */
	CameraGPU(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice);

	CameraGPU(const CameraGPU&) = delete;
	CameraGPU& operator=(const CameraGPU&) = delete;
	CameraGPU(CameraGPU&&) noexcept = default;

	/**
	 * @brief Uploads this frame's camera matrices (host-coherent memcpy).
	 *
	 * Called once per frame by RenderCache::UpdateCamera(), before any pass
	 * records. Passes must not call it: two writers would mean the pass order
	 * decides which camera the frame is drawn with.
	 *
	 * Takes the projection and view matrices separately rather than their product
	 * so the projection stays available on the CPU — DebugPass needs proj[1][1] to
	 * convert a world-space size into pixels, and that factor cannot be recovered
	 * from viewProj once the view rotation is folded in.
	 *
	 * @param proj  Projection matrix (already Y-flipped for Vulkan NDC).
	 * @param view  View matrix.
	 */
	void Update(const glm::mat4& proj, const glm::mat4& view);

	/** @brief The UBO, for a descriptor write. Valid for this object's lifetime. */
	const UniformBuffer<CameraUBOData>& GetUBO() const { return m_ubo; }

	/** @brief Descriptor info for the camera UBO binding. */
	vk::DescriptorBufferInfo GetDescriptorInfo() const { return m_ubo.GetDescriptorInfo(); }

	/** @brief The matrices last uploaded, for passes that also need them on the CPU. */
	const CameraUBOData& GetData() const { return m_data; }

	/** @brief The projection matrix behind GetData().viewProj (never uploaded). */
	const glm::mat4& GetProjection() const { return m_proj; }

	/**
	 * @brief Whether Update() has ever run.
	 *
	 * A pass that reads the UBO before the first Update() would draw with
	 * uninitialized matrices, so consumers check this and skip instead.
	 */
	bool IsValid() const { return m_valid; }

private:
	UniformBuffer<CameraUBOData> m_ubo;
	CameraUBOData m_data{glm::mat4(1.0f), glm::mat4(1.0f)};
	glm::mat4 m_proj{1.0f};
	bool m_valid = false;
};

} // namespace neurus
