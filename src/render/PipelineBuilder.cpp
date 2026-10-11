#include "PipelineBuilder.h"

#include "buffers/BufferLayout.h"

#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Shader stages
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::AddShaderStage(
	const vk::PipelineShaderStageCreateInfo& stageInfo)
{
	p_stages.push_back(stageInfo);
	return *this;
}

// ---------------------------------------------------------------------------
// Vertex input
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetVertexInput(const BufferLayout& layout)
{
	p_vertexBindings.clear();
	p_vertexAttributes.clear();

	p_vertexBindings.push_back(layout.GetBindingDescription());
	p_vertexAttributes = layout.GetAttributeDescriptions();

	p_vertexInput = vk::PipelineVertexInputStateCreateInfo(
		{},
		p_vertexBindings,
		p_vertexAttributes);
	p_vertexInputSet = true;
	return *this;
}

PipelineBuilder& PipelineBuilder::SetVertexInput()
{
	p_vertexBindings.clear();
	p_vertexAttributes.clear();
	p_vertexInput = vk::PipelineVertexInputStateCreateInfo({}, {}, {});
	p_vertexInputSet = true;
	return *this;
}

PipelineBuilder& PipelineBuilder::SetVertexInput(
	const vk::PipelineVertexInputStateCreateInfo& vertexInput)
{
	p_vertexBindings.clear();
	p_vertexAttributes.clear();
	p_vertexInput = vertexInput;
	p_vertexInputSet = true;
	return *this;
}

// ---------------------------------------------------------------------------
// Input assembly
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetInputAssembly(
	vk::PrimitiveTopology topology,
	bool primitiveRestart)
{
	p_inputAssembly = vk::PipelineInputAssemblyStateCreateInfo(
		{}, topology, primitiveRestart ? VK_TRUE : VK_FALSE);
	return *this;
}

// ---------------------------------------------------------------------------
// Rasterization
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetViewMask(uint32_t viewMask)
{
	p_viewMask = viewMask;
	return *this;
}

PipelineBuilder& PipelineBuilder::SetRasterization(
	vk::PolygonMode polygonMode,
	vk::CullModeFlags cullMode,
	vk::FrontFace frontFace,
	float lineWidth)
{
	p_rasterizer = vk::PipelineRasterizationStateCreateInfo(
		{},
		VK_FALSE,  // depthClampEnable
		VK_FALSE,  // rasterizerDiscardEnable
		polygonMode,
		cullMode,
		frontFace,
		VK_FALSE,  // depthBiasEnable
		0.0f,      // depthBiasConstantFactor
		0.0f,      // depthBiasClamp
		0.0f,      // depthBiasSlopeFactor
		lineWidth);
	return *this;
}

// ---------------------------------------------------------------------------
// Multisampling
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetMultisampling(
	vk::SampleCountFlagBits samples,
	bool sampleShadingEnable,
	float minSampleShading)
{
	p_multisample = vk::PipelineMultisampleStateCreateInfo(
		{},
		samples,
		sampleShadingEnable ? VK_TRUE : VK_FALSE,
		minSampleShading);
	return *this;
}

// ---------------------------------------------------------------------------
// Depth / stencil
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetDepthStencil(
	bool depthTest,
	bool depthWrite,
	vk::CompareOp compareOp,
	bool stencilTest)
{
	vk::StencilOpState stencilOpState(
		vk::StencilOp::eKeep,
		vk::StencilOp::eKeep,
		vk::StencilOp::eKeep,
		vk::CompareOp::eAlways,
		0, 0, 0);

	p_depthStencil = vk::PipelineDepthStencilStateCreateInfo(
		{},
		depthTest ? VK_TRUE : VK_FALSE,
		depthWrite ? VK_TRUE : VK_FALSE,
		compareOp,
		VK_FALSE,  // depthBoundsTestEnable
		stencilTest ? VK_TRUE : VK_FALSE,
		stencilOpState,  // front
		stencilOpState,  // back
		0.0f, 0.0f);     // min/max depth bounds
	return *this;
}

PipelineBuilder& PipelineBuilder::ClearDepthStencil()
{
	p_depthStencil.reset();
	return *this;
}

// ---------------------------------------------------------------------------
// Color blending
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::AddColorBlendAttachment(
	const vk::PipelineColorBlendAttachmentState& attachment)
{
	p_colorBlendAttachments.push_back(attachment);
	return *this;
}

PipelineBuilder& PipelineBuilder::SetColorBlendAttachment()
{
	vk::PipelineColorBlendAttachmentState attachment;
	attachment.blendEnable = VK_TRUE;
	attachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
	attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	attachment.colorBlendOp = vk::BlendOp::eAdd;
	attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	attachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
	attachment.alphaBlendOp = vk::BlendOp::eAdd;
	attachment.colorWriteMask =
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	p_colorBlendAttachments.clear();
	p_colorBlendAttachments.push_back(attachment);
	return *this;
}

PipelineBuilder& PipelineBuilder::ClearColorBlendAttachments()
{
	p_colorBlendAttachments.clear();
	return *this;
}

// ---------------------------------------------------------------------------
// Dynamic state
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::AddDynamicState(vk::DynamicState state)
{
	p_dynamicStates.push_back(state);
	return *this;
}

// ---------------------------------------------------------------------------
// Descriptor set layouts
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetDescriptorSetLayouts(
	const std::vector<vk::DescriptorSetLayout>& layouts)
{
	p_descriptorSetLayouts = layouts;
	return *this;
}

PipelineBuilder& PipelineBuilder::AddDescriptorSetLayout(vk::DescriptorSetLayout layout)
{
	p_descriptorSetLayouts.push_back(layout);
	return *this;
}

// ---------------------------------------------------------------------------
// Push constant ranges
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetPushConstantRanges(
	const std::vector<vk::PushConstantRange>& ranges)
{
	p_pushConstantRanges = ranges;
	return *this;
}

PipelineBuilder& PipelineBuilder::AddPushConstantRange(const vk::PushConstantRange& range)
{
	p_pushConstantRanges.push_back(range);
	return *this;
}

// ---------------------------------------------------------------------------
// Pipeline cache
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetPipelineCache(const vk::raii::PipelineCache* cache)
{
	p_pipelineCache = cache ? **cache : VK_NULL_HANDLE;
	return *this;
}

// ---------------------------------------------------------------------------
// Dynamic rendering attachment formats
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetColorFormats(const std::vector<vk::Format>& formats)
{
	p_colorFormats = formats;
	return *this;
}

PipelineBuilder& PipelineBuilder::SetDepthFormat(vk::Format format)
{
	p_depthFormat = format;
	return *this;
}

PipelineBuilder& PipelineBuilder::SetStencilFormat(vk::Format format)
{
	p_stencilFormat = format;
	return *this;
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

PipelineBuilder& PipelineBuilder::SetDebugName(const char* name)
{
	p_debugName = name ? name : "";
	return *this;
}

// ---------------------------------------------------------------------------
// BuildGraphicsPipeline
// ---------------------------------------------------------------------------

Pipeline PipelineBuilder::BuildGraphicsPipeline(const vk::raii::Device& device)
{
	// --- Validate required fields ---
	if (p_stages.empty())
	{
		throw std::runtime_error(
			"PipelineBuilder::BuildGraphicsPipeline: "
			"no shader stages added - call AddShaderStage() at least once.");
	}

	if (p_colorFormats.empty() && !p_depthFormat.has_value())
	{
		throw std::runtime_error(
			"PipelineBuilder::BuildGraphicsPipeline: "
			"no color or depth format set - call SetColorFormats() or SetDepthFormat().");
	}

	// --- Viewport state (dynamic - count non-zero, pointers null) ---
	vk::PipelineViewportStateCreateInfo viewportState({}, 1, nullptr, 1, nullptr);

	// --- Dynamic state ---
	p_dynamicState = vk::PipelineDynamicStateCreateInfo({}, p_dynamicStates);

	// --- Color blend state ---
	if (!p_colorBlendAttachments.empty())
	{
		p_colorBlend = vk::PipelineColorBlendStateCreateInfo(
			{}, VK_FALSE,
			vk::LogicOp::eCopy,
			p_colorBlendAttachments);
	}
	else
	{
		p_colorBlend = vk::PipelineColorBlendStateCreateInfo({}, VK_FALSE,
			vk::LogicOp::eCopy, nullptr);
	}

	// --- Pipeline layout ---
	vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo(
		{},
		p_descriptorSetLayouts,
		p_pushConstantRanges);

	vk::raii::PipelineLayout pipelineLayout(device, pipelineLayoutCreateInfo);

	// --- Dynamic rendering pipeline create info ---
	vk::PipelineRenderingCreateInfo renderingCreateInfo(
		p_viewMask,
		p_colorFormats,
		p_depthFormat.value_or(vk::Format::eUndefined),
		p_stencilFormat.value_or(vk::Format::eUndefined));

	// --- Pointers to optional state structs ---
	const vk::PipelineVertexInputStateCreateInfo* pVertexInput =
		p_vertexInputSet ? &p_vertexInput : nullptr;

	const vk::PipelineDepthStencilStateCreateInfo* pDepthStencil =
		p_depthStencil.has_value() ? &p_depthStencil.value() : nullptr;

	// --- Assemble graphics pipeline ---
	vk::GraphicsPipelineCreateInfo pipelineCreateInfo(
		{},
		p_stages,
		pVertexInput,
		&p_inputAssembly,
		nullptr,       // No tessellation
		&viewportState,
		&p_rasterizer,
		&p_multisample,
		pDepthStencil,
		&p_colorBlend,
		&p_dynamicState,
		*pipelineLayout,
		nullptr,       // No render pass (dynamic rendering)
		0,             // Subpass index (unused)
		nullptr,       // No base pipeline
		-1,            // No base pipeline index
		&renderingCreateInfo);

	auto pipeline = vk::raii::Pipeline(device, nullptr, pipelineCreateInfo);

#ifdef _DEBUG
	if (!p_debugName.empty())
	{
		device.setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT(
			vk::ObjectType::ePipeline,
			reinterpret_cast<uint64_t>(static_cast<VkPipeline>(*pipeline)),
			p_debugName.c_str()));
	}
#endif

	return Pipeline{std::move(pipeline), std::move(pipelineLayout),
	                PipelineType::Geometry};
}

// ---------------------------------------------------------------------------
// BuildComputePipeline
// ---------------------------------------------------------------------------

Pipeline PipelineBuilder::BuildComputePipeline(const vk::raii::Device& device)
{
	if (p_stages.empty())
	{
		throw std::runtime_error(
			"PipelineBuilder::BuildComputePipeline: "
			"no shader stages added - call AddShaderStage() at least once.");
	}

	// --- Pipeline layout ---
	vk::PipelineLayoutCreateInfo layoutCreateInfo(
		{},
		p_descriptorSetLayouts,
		p_pushConstantRanges);

	vk::raii::PipelineLayout pipelineLayout(device, layoutCreateInfo);

	// --- Create compute pipeline (first stage is the compute shader) ---
	vk::ComputePipelineCreateInfo computeCreateInfo(
		{},                // flags
		p_stages[0],       // stage
		*pipelineLayout    // layout
	);

	auto pipeline = vk::raii::Pipeline(device, nullptr, computeCreateInfo);

#ifdef _DEBUG
	if (!p_debugName.empty())
	{
		device.setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT(
			vk::ObjectType::ePipeline,
			reinterpret_cast<uint64_t>(static_cast<VkPipeline>(*pipeline)),
			p_debugName.c_str()));
	}
#endif

	return Pipeline{std::move(pipeline), std::move(pipelineLayout),
	                PipelineType::Compute};
}

} // namespace neurus
