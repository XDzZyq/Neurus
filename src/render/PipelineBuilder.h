#pragma once

#include "Pipeline.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neurus {

class BufferLayout;

// ---------------------------------------------------------------------------
// PipelineBuilder - Fluent API for constructing graphics AND compute pipelines
// ---------------------------------------------------------------------------

/**
 * @brief Fluent builder for VkGraphicsPipelineCreateInfo (with
 *        VK_KHR_dynamic_rendering) and VkComputePipelineCreateInfo.
 *
 * Accumulates state via chained method calls and produces a Pipeline struct
 * (which owns both the vk::raii::Pipeline and its vk::raii::PipelineLayout)
 * on BuildGraphicsPipeline() or BuildComputePipeline().
 */
class PipelineBuilder
{
public:
	PipelineBuilder() = default;

	// -----------------------------------------------------------------------
	// Shader stages
	// -----------------------------------------------------------------------

	/**
	 * @brief Adds a pre-built shader stage create-info.
	 * @param stageInfo   Fully populated VkPipelineShaderStageCreateInfo.
	 * @return *this for chaining.
	 */
	PipelineBuilder& AddShaderStage(const vk::PipelineShaderStageCreateInfo& stageInfo);

	// -----------------------------------------------------------------------
	// Vertex input
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets vertex input state from a BufferLayout.
	 *
	 * @param layout  Vertex attribute layout describing bindings + attributes.
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetVertexInput(const BufferLayout& layout);

	/**
	 * @brief Clears vertex input state (no vertex buffers / attributes).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetVertexInput();

	/**
	 * @brief Sets vertex input state from a raw VkPipelineVertexInputStateCreateInfo.
	 * @param vertexInput  Pre-built vertex input state.
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetVertexInput(const vk::PipelineVertexInputStateCreateInfo& vertexInput);

	// -----------------------------------------------------------------------
	// Input assembly
	// -----------------------------------------------------------------------

	/**
	 * @brief Configures input assembly (primitive topology + restart).
	 *
	 * @param topology         Primitive topology (default: eTriangleList).
	 * @param primitiveRestart Enable primitive restart (default: false).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetInputAssembly(
		vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList,
		bool primitiveRestart = false);

	// -----------------------------------------------------------------------
	// Rasterization
	// -----------------------------------------------------------------------
	/**
	 * @brief Sets the view mask for multiview rendering.
	 * 
	 * @param viewMask  Bitmask of views to render to (e.g., 0 for single view, 0x3f for cubemap views).
	 */
	PipelineBuilder& SetViewMask(uint32_t viewMask);

	/**
	 * @brief Configures the rasterization state.
	 *
	 * @param polygonMode Polygon fill mode (default: eFill).
	 * @param cullMode    Face culling mode (default: eNone).
	 * @param frontFace   Front-face winding (default: eClockwise).
	 * @param lineWidth   Line width for wireframe (default: 1.0f).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetRasterization(
		vk::PolygonMode polygonMode = vk::PolygonMode::eFill,
		vk::CullModeFlags cullMode = vk::CullModeFlagBits::eNone,
		vk::FrontFace frontFace = vk::FrontFace::eClockwise,
		float lineWidth = 1.0f);

	// -----------------------------------------------------------------------
	// Multisampling
	// -----------------------------------------------------------------------

	/**
	 * @brief Configures multisampling state.
	 *
	 * @param samples              Sample count (default: e1).
	 * @param sampleShadingEnable  Enable per-sample shading (default: false).
	 * @param minSampleShading     Minimum fraction of sample shading (default: 0.0f).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetMultisampling(
		vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1,
		bool sampleShadingEnable = false,
		float minSampleShading = 0.0f);

	// -----------------------------------------------------------------------
	// Depth / stencil
	// -----------------------------------------------------------------------

	/**
	 * @brief Configures depth/stencil testing.
	 *
	 * @param depthTest    Enable depth testing.
	 * @param depthWrite   Enable depth writes.
	 * @param compareOp    Depth comparison operator (default: eLess).
	 * @param stencilTest  Enable stencil testing (default: false).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetDepthStencil(
		bool depthTest,
		bool depthWrite,
		vk::CompareOp compareOp = vk::CompareOp::eLess,
		bool stencilTest = false);

	/**
	 * @brief Removes the depth/stencil state.
	 *
	 * After calling this, the pipeline will have no depth/stencil attachment.
	 * @return *this for chaining.
	 */
	PipelineBuilder& ClearDepthStencil();

	// -----------------------------------------------------------------------
	// Color blending
	// -----------------------------------------------------------------------

	/**
	 * @brief Appends one color blend attachment state.
	 *
	 * @param attachment  VkPipelineColorBlendAttachmentState.
	 * @return *this for chaining.
	 */
	PipelineBuilder& AddColorBlendAttachment(
		const vk::PipelineColorBlendAttachmentState& attachment);

	/**
	 * @brief Sets color blending with standard alpha blending for a single attachment.
	 *
	 * BlendEnable = true, SrcAlphaBlendFactor = eSrcAlpha,
	 * DstAlphaBlendFactor = eOneMinusSrcAlpha, standard write mask.
	 *
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetColorBlendAttachment();

	/**
	 * @brief Clears all color blend attachments.
	 * @return *this for chaining.
	 */
	PipelineBuilder& ClearColorBlendAttachments();

	// -----------------------------------------------------------------------
	// Dynamic state
	// -----------------------------------------------------------------------

	/**
	 * @brief Adds an additional dynamic state beyond viewport + scissor.
	 *
	 * Viewport and scissor are always dynamic and need not be added here.
	 *
	 * @param state  VkDynamicState to add.
	 * @return *this for chaining.
	 */
	PipelineBuilder& AddDynamicState(vk::DynamicState state);

	// -----------------------------------------------------------------------
	// Descriptor set layouts
	// -----------------------------------------------------------------------

	/**
	 * @brief Replaces all descriptor set layouts (clear + set).
	 *
	 * @param layouts  Vector of raw VkDescriptorSetLayout handles.
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetDescriptorSetLayouts(
		const std::vector<vk::DescriptorSetLayout>& layouts);

	/**
	 * @brief Appends one descriptor set layout.
	 *
	 * @param layout  Raw VkDescriptorSetLayout handle.
	 * @return *this for chaining.
	 */
	PipelineBuilder& AddDescriptorSetLayout(vk::DescriptorSetLayout layout);

	// -----------------------------------------------------------------------
	// Push constant ranges
	// -----------------------------------------------------------------------

	/**
	 * @brief Replaces all push-constant ranges (clear + set).
	 *
	 * @param ranges  Vector of VkPushConstantRange entries.
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetPushConstantRanges(
		const std::vector<vk::PushConstantRange>& ranges);

	/**
	 * @brief Appends one push-constant range.
	 *
	 * @param range  Push constant range descriptor.
	 * @return *this for chaining.
	 */
	PipelineBuilder& AddPushConstantRange(const vk::PushConstantRange& range);

	// -----------------------------------------------------------------------
	// Pipeline cache
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets an optional pipeline cache for reuse across builds.
	 * @param cache  Pointer to an existing vk::raii::PipelineCache (may be nullptr).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetPipelineCache(const vk::raii::PipelineCache* cache);

	// -----------------------------------------------------------------------
	// Dynamic rendering attachment formats
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets the color attachment formats for dynamic rendering.
	 *
	 * Must be called - one format per color attachment referenced by the
	 * pipeline.
	 *
	 * @param formats  Vector of VkFormat per color attachment.
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetColorFormats(const std::vector<vk::Format>& formats);

	/**
	 * @brief Sets the depth attachment format for dynamic rendering.
	 * @param format  Depth format (e.g. eD32Sfloat).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetDepthFormat(vk::Format format);

	/**
	 * @brief Sets the stencil attachment format for dynamic rendering.
	 * @param format  Stencil format (e.g. eS8Uint).
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetStencilFormat(vk::Format format);

	// -----------------------------------------------------------------------
	// Debug
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets a debug name for the pipeline (applied inside Build*Pipeline).
	 *
	 * The name is assigned to the VkPipeline object via
	 * vkSetDebugUtilsObjectNameEXT in Debug builds.
	 *
	 * @param name  Human-readable debug name (e.g. "GeometryPass::G-Buffer").
	 * @return *this for chaining.
	 */
	PipelineBuilder& SetDebugName(const char* name);

	// -----------------------------------------------------------------------
	// Build (graphics)
	// -----------------------------------------------------------------------

	/**
	 * @brief Builds the graphics pipeline from accumulated state.
	 *
	 * Creates the pipeline layout from the descriptor set layouts and
	 * push-constant ranges, and appends VkPipelineRenderingCreateInfo for
	 * dynamic rendering.
	 *
	 * @param device  Logical device.
	 * @return Fully constructed Pipeline (type = Geometry).
	 * @throws std::runtime_error if required fields are missing or invalid.
	 */
	Pipeline BuildGraphicsPipeline(const vk::raii::Device& device);

	// -----------------------------------------------------------------------
	// Build (compute)
	// -----------------------------------------------------------------------

	/**
	 * @brief Builds the compute pipeline from accumulated state.
	 *
	 * Creates the pipeline layout from the descriptor set layouts and
	 * push-constant ranges; the returned Pipeline owns both.
	 *
	 * @param device  Logical device.
	 * @return Fully constructed Pipeline (type = Compute).
	 * @throws std::runtime_error if no shader stage is set.
	 */
	Pipeline BuildComputePipeline(const vk::raii::Device& device);

private:
	// --- Shader stages ---
	std::vector<vk::PipelineShaderStageCreateInfo> p_stages;

	// --- Vertex input ---
	vk::PipelineVertexInputStateCreateInfo p_vertexInput = {};
	std::vector<vk::VertexInputBindingDescription> p_vertexBindings;
	std::vector<vk::VertexInputAttributeDescription> p_vertexAttributes;
	bool p_vertexInputSet = false;

	// --- Input assembly ---
	vk::PipelineInputAssemblyStateCreateInfo p_inputAssembly =
		vk::PipelineInputAssemblyStateCreateInfo(
			{}, vk::PrimitiveTopology::eTriangleList, VK_FALSE);

	// --- Rasterization ---
	vk::PipelineRasterizationStateCreateInfo p_rasterizer =
		vk::PipelineRasterizationStateCreateInfo(
			{}, VK_FALSE, VK_FALSE,
			vk::PolygonMode::eFill,
			vk::CullModeFlagBits::eNone,
			vk::FrontFace::eClockwise,
			VK_FALSE, 0.0f, 0.0f, 0.0f, 1.0f);

	// --- Multisampling ---
	vk::PipelineMultisampleStateCreateInfo p_multisample =
		vk::PipelineMultisampleStateCreateInfo(
			{}, vk::SampleCountFlagBits::e1);

	// --- Depth / stencil ---
	std::optional<vk::PipelineDepthStencilStateCreateInfo> p_depthStencil;

	// --- Color blending ---
	std::vector<vk::PipelineColorBlendAttachmentState> p_colorBlendAttachments;
	vk::PipelineColorBlendStateCreateInfo p_colorBlend;

	// --- Dynamic states ---
	std::vector<vk::DynamicState> p_dynamicStates = {
		vk::DynamicState::eViewport,
		vk::DynamicState::eScissor,
	};
	vk::PipelineDynamicStateCreateInfo p_dynamicState;

	// --- Pipeline layout ---
	std::vector<vk::DescriptorSetLayout> p_descriptorSetLayouts;
	std::vector<vk::PushConstantRange> p_pushConstantRanges;

	// --- Pipeline cache ---
	vk::PipelineCache p_pipelineCache = VK_NULL_HANDLE;

	// --- Dynamic rendering ---
	std::vector<vk::Format> p_colorFormats;
	std::optional<vk::Format> p_depthFormat;
	std::optional<vk::Format> p_stencilFormat;

	uint32_t p_viewMask = 0;

	// --- Debug ---
	std::string p_debugName;
};

} // namespace neurus
