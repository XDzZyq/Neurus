/**
 * @file PassFactory.cpp
 * @brief Built-in pass construction and port cache.
 */

#include "PassFactory.hpp"
#include "shader/BuiltInRenderData.hpp"

#include "../passes/ComposePass.hpp"
#include "../passes/FXAAPass.hpp"
#include "../passes/GeometryPass.hpp"
#include "../passes/LightingPass.hpp"
#include "../passes/SSAOPass.hpp"
#include "../passes/ShadowDepthPass.hpp"
#include "../passes/ShadowIntensityPass.hpp"

#include <stdexcept>
#include <utility>

namespace neurus {

PassFactory::PassFactory(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           const uint32_t numSets,
                           SharedQueue& graphicsQueue,
                           const uint32_t queueFamilyIndex)
	: m_device(device), m_physicalDevice(physicalDevice), m_numSets(numSets),
	  m_graphicsQueue(graphicsQueue), m_queueFamilyIndex(queueFamilyIndex)
{
}

PassFactory::~PassFactory() = default;

void PassFactory::Add(std::string kind, std::unique_ptr<Pass> pass)
{
	m_passes.emplace(std::move(kind), std::move(pass));
}

Pass* PassFactory::Find(const std::string_view kind)
{
	const std::string key(kind);
	if (const auto found = m_passes.find(key); found != m_passes.end())
		return found->second.get();
	if (kind == "GeometryPass")
		Add(key, std::make_unique<GeometryPass>(m_device, m_physicalDevice));
	else if (kind == "LightingPass")
		Add(key, std::make_unique<LightingPass>(m_device, m_physicalDevice, m_numSets));
	else if (kind == "SSAOPass")
		Add(key, std::make_unique<SSAOPass>(m_device, m_physicalDevice, m_numSets,
			m_graphicsQueue, m_queueFamilyIndex));
	else if (kind == "ShadowDepthPass")
		Add(key, std::make_unique<ShadowDepthPass>(m_device, m_physicalDevice,
			m_graphicsQueue, m_queueFamilyIndex, ShadowDepthPass::kDefaultResolution));
	else if (kind == "ShadowIntensityPass")
		Add(key, std::make_unique<ShadowIntensityPass>(m_device, m_physicalDevice, m_numSets));
	else if (kind == "ComposePass")
		Add(key, std::make_unique<ComposePass>(m_device, m_physicalDevice, m_numSets));
	else if (kind == "FXAAPass")
		Add(key, std::make_unique<FXAAPass>(m_device, m_physicalDevice, m_numSets));
	else
		return nullptr;
	return m_passes.at(key).get();
}

const PassPorts* PassFactory::FindPassPorts(const std::string_view passKind) const
{
	return BuiltInRenderData::Get().Ports().FindPassPorts(passKind);
}

std::vector<std::string> PassFactory::ExternalInputs(const std::string_view passKind) const
{
	return BuiltInRenderData::Get().Ports().ExternalInputs(passKind);
}

const PassPorts& PassFactory::Ports(const std::string_view passKind) const
{
	if (const PassPorts* ports = FindPassPorts(passKind))
		return *ports;
	throw std::invalid_argument("PassFactory::Ports: unknown pass kind '" + std::string(passKind) + "'");
}

GeometryPass* PassFactory::Geometry()
{
	return static_cast<GeometryPass*>(Find("GeometryPass"));
}

LightingPass* PassFactory::Lighting()
{
	return static_cast<LightingPass*>(Find("LightingPass"));
}

SSAOPass* PassFactory::SSAO()
{
	return static_cast<SSAOPass*>(Find("SSAOPass"));
}

ShadowDepthPass* PassFactory::ShadowDepth()
{
	return static_cast<ShadowDepthPass*>(Find("ShadowDepthPass"));
}

ShadowIntensityPass* PassFactory::ShadowIntensity()
{
	return static_cast<ShadowIntensityPass*>(Find("ShadowIntensityPass"));
}

ComposePass* PassFactory::Compose()
{
	return static_cast<ComposePass*>(Find("ComposePass"));
}

FXAAPass* PassFactory::FXAA()
{
	return static_cast<FXAAPass*>(Find("FXAAPass"));
}

} // namespace neurus
