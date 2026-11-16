/**
 * @file PassRegistry.cpp
 * @brief Built-in pass construction and port cache.
 */

#include "PassRegistry.hpp"

#include "../passes/ComposePass.hpp"
#include "../passes/FXAAPass.hpp"
#include "../passes/GeometryPass.hpp"
#include "../passes/LightingPass.hpp"
#include "../passes/SSAOPass.hpp"
#include "../passes/ShadowDepthPass.hpp"
#include "../passes/ShadowIntensityPass.hpp"

#include "asset/data/ShaderData.hpp"

#include <stdexcept>
#include <utility>

namespace neurus {

PassRegistry::PassRegistry(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           const uint32_t numSets,
                           SharedQueue& graphicsQueue,
                           const uint32_t queueFamilyIndex)
{
	// Same construction order the renderer used before the registry existed.
	Add("GeometryPass", std::make_unique<GeometryPass>(device, physicalDevice));
	Add("LightingPass", std::make_unique<LightingPass>(device, physicalDevice, numSets));
	Add("SSAOPass",
	    std::make_unique<SSAOPass>(device, physicalDevice, numSets,
	                               graphicsQueue, queueFamilyIndex));
	Add("ShadowDepthPass",
	    std::make_unique<ShadowDepthPass>(device, physicalDevice,
	                                      graphicsQueue, queueFamilyIndex,
	                                      ShadowDepthPass::kDefaultResolution));
	Add("ShadowIntensityPass",
	    std::make_unique<ShadowIntensityPass>(device, physicalDevice, numSets));
	Add("ComposePass", std::make_unique<ComposePass>(device, physicalDevice, numSets));
	Add("FXAAPass", std::make_unique<FXAAPass>(device, physicalDevice, numSets));
}

PassRegistry::~PassRegistry() = default;

void PassRegistry::Add(std::string kind, std::unique_ptr<Pass> pass)
{
	Pass* raw = pass.get();

	const std::vector<const ShaderData*> shaders = raw->GetShaders();
	const std::vector<ShaderPort> declared = raw->DeclaredOutputs();

	m_passes.emplace(kind, std::move(pass));
	m_ports.emplace(kind, ExtractPassPorts(kind, shaders, declared));
	m_externals.emplace(kind, raw->ExternalInputPorts());
}

Pass* PassRegistry::Find(const std::string_view kind)
{
	const auto it = m_passes.find(std::string(kind));
	return it == m_passes.end() ? nullptr : it->second.get();
}

const PassPorts* PassRegistry::FindPassPorts(const std::string_view passKind) const
{
	const auto it = m_ports.find(std::string(passKind));
	return it == m_ports.end() ? nullptr : &it->second;
}

std::vector<std::string> PassRegistry::ExternalInputs(const std::string_view passKind) const
{
	const auto it = m_externals.find(std::string(passKind));
	return it == m_externals.end() ? std::vector<std::string>{} : it->second;
}

const PassPorts& PassRegistry::Ports(const std::string_view passKind) const
{
	const auto it = m_ports.find(std::string(passKind));
	if (it == m_ports.end())
	{
		throw std::invalid_argument(
			"PassRegistry::Ports: unknown pass kind '" + std::string(passKind) + "'");
	}
	return it->second;
}

GeometryPass* PassRegistry::Geometry()
{
	return static_cast<GeometryPass*>(Find("GeometryPass"));
}

LightingPass* PassRegistry::Lighting()
{
	return static_cast<LightingPass*>(Find("LightingPass"));
}

SSAOPass* PassRegistry::SSAO()
{
	return static_cast<SSAOPass*>(Find("SSAOPass"));
}

ShadowDepthPass* PassRegistry::ShadowDepth()
{
	return static_cast<ShadowDepthPass*>(Find("ShadowDepthPass"));
}

ShadowIntensityPass* PassRegistry::ShadowIntensity()
{
	return static_cast<ShadowIntensityPass*>(Find("ShadowIntensityPass"));
}

ComposePass* PassRegistry::Compose()
{
	return static_cast<ComposePass*>(Find("ComposePass"));
}

FXAAPass* PassRegistry::FXAA()
{
	return static_cast<FXAAPass*>(Find("FXAAPass"));
}

} // namespace neurus
