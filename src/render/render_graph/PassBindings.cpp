/**
 * @file PassBindings.cpp
 * @brief Binding-plan construction and descriptor writes shared by graph and tests.
 */

#include "render_graph/PassBindings.hpp"

#include "../DescriptorManager.hpp"
#include "../Image.hpp"

#include <algorithm>

namespace neurus {

std::vector<GraphImageBinding> BuildImageBindings(
	const PassPorts& ports,
	const Pass& pass,
	const std::function<bool(std::string_view)>& connected)
{
	std::vector<GraphImageBinding> bindings;
	const std::vector<uint32_t> selfManaged = pass.SelfManagedBindings();

	const auto isSelfManaged = [&selfManaged](const uint32_t binding)
	{
		return std::find(selfManaged.begin(), selfManaged.end(), binding) != selfManaged.end();
	};

	for (const ShaderPort& input : ports.Inputs())
	{
		if (!IsSamplerPortType(input.type))
			continue;
		if (connected && !connected(input.name))
			continue;
		if (isSelfManaged(input.binding))
			continue;

		bindings.push_back({
			input.name,
			PortDirection::Input,
			input.binding,
			vk::DescriptorType::eCombinedImageSampler,
			vk::ImageLayout::eShaderReadOnlyOptimal,
		});
	}

	for (const ShaderPort& output : ports.Outputs())
	{
		if (!IsStorageImagePortType(output.type))
			continue;
		if (isSelfManaged(output.binding))
			continue;

		bindings.push_back({
			output.name,
			PortDirection::Output,
			output.binding,
			vk::DescriptorType::eStorageImage,
			vk::ImageLayout::eGeneral,
		});
	}

	return bindings;
}

void WritePassBindings(Pass& pass,
                       RenderCache& cache,
                       const RenderContext& ctx,
                       const PassResources& resources,
                       const std::span<const GraphImageBinding> bindings)
{
	DescriptorSet* set = pass.GraphDescriptorSet(ctx.frameIndex);
	if (!set)
		return;

	const vk::Sampler sampler = pass.GraphSampler();

	for (const GraphImageBinding& binding : bindings)
	{
		const Image* image = binding.direction == PortDirection::Input
		                         ? resources.Input(binding.port)
		                         : resources.Output(binding.port);
		if (!image)
			continue;

		const vk::Sampler bound =
			binding.type == vk::DescriptorType::eCombinedImageSampler
				? sampler
				: vk::Sampler{};

		set->WriteImage(binding.binding,
		                vk::DescriptorImageInfo(bound, *image->ImageViewHandle(),
		                                        binding.layout),
		                binding.type);
	}

	std::vector<GraphBufferBinding> buffers;
	pass.CollectGraphBuffers(cache, ctx.frameIndex, buffers);
	for (const GraphBufferBinding& buffer : buffers)
	{
		if (buffer.info.buffer)
			set->WriteBuffer(buffer.binding, buffer.info, buffer.type);
	}

	std::vector<GraphExternalImage> externals;
	pass.CollectGraphExternalImages(cache, ctx, externals);
	for (const GraphExternalImage& external : externals)
	{
		if (external.info.imageView)
			set->WriteImage(external.binding, external.info, external.type);
	}
}

} // namespace neurus
