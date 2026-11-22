/**
 * @file PassFactory.cpp
 * @brief Construction and reuse of runtime GPU passes.
 */

#include "PassFactory.hpp"
#include "render/RenderCache.hpp"
#include "asset/data/render_graph/PassKinds.hpp"

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
                           const uint32_t queueFamilyIndex,
                           RenderCache& cache)
	: m_cache(cache)
	, m_device(device)
	, m_physicalDevice(physicalDevice)
	, m_numSets(numSets)
	, m_graphicsQueue(graphicsQueue)
	, m_queueFamilyIndex(queueFamilyIndex)
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
	{
		return found->second.get();
	}
	auto pass = Create(kind, ShaderFor(kind));
	if (!pass)
	{
		return nullptr;
	}
	Add(key, std::move(pass));
	return m_passes.at(key).get();
}

std::shared_ptr<const ShaderGPU> PassFactory::ShaderFor(std::string_view kind, std::optional<int> shaderId)
{
	if (shaderId)
	{
		auto shader = m_cache.GetShader(*shaderId);
		if (!shader)
		{
			throw std::invalid_argument("shader is not uploaded: " + std::to_string(*shaderId));
		}
		return shader;
	}
	const auto* info = FindPassKind(kind);
	if (!info || info->shaders.empty())
	{
		return nullptr;
	}
	return m_cache.EnsureDefaultShader(info->shaders.front().name);
}

std::unique_ptr<Pass> PassFactory::Create(std::string_view kind,
                                          std::shared_ptr<const ShaderGPU> shader)
{
	if (kind == "GeometryPass")
	{
		return std::make_unique<GeometryPass>(m_device, m_physicalDevice, std::move(shader));
	}
	if (kind == "LightingPass")
	{
		return std::make_unique<LightingPass>(m_device, m_physicalDevice, m_numSets, std::move(shader));
	}
	if (kind == "SSAOPass")
	{
		return std::make_unique<SSAOPass>(m_device, m_physicalDevice, m_numSets,
		                                  m_graphicsQueue, m_queueFamilyIndex, std::move(shader));
	}
	if (kind == "ShadowDepthPass")
	{
		const auto sunShader = m_cache.EnsureDefaultShader(FindPassKind(kind)->shaders[1].name);
		return std::make_unique<ShadowDepthPass>(m_device, m_physicalDevice, m_graphicsQueue,
		                                         m_queueFamilyIndex, ShadowDepthPass::kDefaultResolution,
		                                         std::move(shader), sunShader);
	}
	if (kind == "ShadowIntensityPass")
	{
		const auto sunShader = m_cache.EnsureDefaultShader(FindPassKind(kind)->shaders[1].name);
		return std::make_unique<ShadowIntensityPass>(m_device, m_physicalDevice, m_numSets,
		                                             std::move(shader), sunShader);
	}
	if (kind == "ComposePass")
	{
		return std::make_unique<ComposePass>(m_device, m_physicalDevice, m_numSets, std::move(shader));
	}
	if (kind == "FXAAPass")
	{
		return std::make_unique<FXAAPass>(m_device, m_physicalDevice, m_numSets, std::move(shader));
	}
	return nullptr;
}

std::shared_ptr<Pass> PassFactory::Acquire(std::string_view kind, std::string_view node, std::optional<int> shaderId)
{
	if (!shaderId)
	{
		return {};
	}
	auto shader = ShaderFor(kind, shaderId);
	for (auto it = m_customPasses.begin(); it != m_customPasses.end();)
	{
		if (it->second.expired())
		{
			it = m_customPasses.erase(it);
		}
		else
		{
			++it;
		}
	}
	const std::string key = std::string(kind) + "/" + std::string(node) + ":"
	                      + std::to_string(*shaderId) + ":" + std::to_string(m_cache.ShaderGeneration());
	if (const auto found = m_customPasses.find(key); found != m_customPasses.end())
	{
		return found->second.lock();
	}
	std::shared_ptr<Pass> pass = Create(kind, std::move(shader));
	if (pass)
	{
		m_customPasses.emplace(key, pass);
	}
	return pass;
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
