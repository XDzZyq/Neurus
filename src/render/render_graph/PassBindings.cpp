/**
 * @file PassBindings.cpp
 * @brief Binding-plan construction and descriptor writes shared by graph and tests.
 */

#include "render_graph/PassBindings.hpp"

#include "../DescriptorManager.hpp"
#include "../Image.hpp"

#include <algorithm>

namespace neurus {

void AppendImageBinding(std::vector<GraphImageBinding>& bindings, std::string_view name,
                        PortDirection direction, std::string_view type, uint32_t binding,
                        std::span<const uint32_t> selfManaged)
{
	if (std::ranges::find(selfManaged, binding) != selfManaged.end())
		return;
	if (direction == PortDirection::Input && IsSamplerPortType(type))
	{
		bindings.push_back({std::string(name), direction, binding, vk::DescriptorType::eCombinedImageSampler,
		                    vk::ImageLayout::eShaderReadOnlyOptimal});
	}
	else if (direction == PortDirection::Output && IsStorageImagePortType(type))
	{
		bindings.push_back({std::string(name), direction, binding, vk::DescriptorType::eStorageImage,
		                    vk::ImageLayout::eGeneral});
	}
}

std::vector<GraphImageBinding> BuildImageBindings(
	const CPUPorts& ports,
	const Pass& pass,
	const std::function<bool(std::string_view)>& connected)
{
	std::vector<GraphImageBinding> bindings;
	const auto selfManaged = pass.SelfManagedBindings();
	for (const auto& input : ports.Inputs())
		if (IsSamplerPortType(input.type) && (!connected || connected(input.name)))
			AppendImageBinding(bindings, input.name, input.direction, input.type, input.binding, selfManaged);
	for (const auto& output : ports.Outputs())
		AppendImageBinding(bindings, output.name, output.direction, output.type, output.binding, selfManaged);
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
