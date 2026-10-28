/**
 * @file LightingCache.cpp
 * @brief Light SSBO storage implementation using ArrayBuffer<T>.
 */

#include "render/resources/LightingCache.h"

#include "core/Log.h"

#include <cstdint>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LightingCache::LightingCache(const vk::raii::Device& device,
                         const vk::raii::PhysicalDevice& physicalDevice,
                         SharedQueue& graphicsQueue,
                         uint32_t queueFamilyIndex)
	: m_pointLightSSBO(device, physicalDevice, graphicsQueue, queueFamilyIndex,
	                   vk::BufferUsageFlagBits::eStorageBuffer, "PointLightSSBO")
	, m_sunLightSSBO(device, physicalDevice, graphicsQueue, queueFamilyIndex,
	                 vk::BufferUsageFlagBits::eStorageBuffer, "SunLightSSBO")
	, m_spotLightSSBO(device, physicalDevice, graphicsQueue, queueFamilyIndex,
	                  vk::BufferUsageFlagBits::eStorageBuffer, "SpotLightSSBO")
{
	NEURUS_LOG("[LightingCache] Created");
}

// ---------------------------------------------------------------------------
// Point light SSBO
// ---------------------------------------------------------------------------

void LightingCache::UpdatePointLights(const std::vector<PointLightStruct>& lights)
{
	m_pointLightSSBO.Upload(lights);
	NEURUS_LOG("[LightingCache] Uploaded " << m_pointLightSSBO.size() << " point lights");
}

void LightingCache::UpdatePointLight(const PointLightStruct& light, uint32_t index)
{
	m_pointLightSSBO.Update(light, index);
}

const GPUBuffer* LightingCache::GetPointLightSSBO() const
{
	return m_pointLightSSBO.gpuBuffer();
}

uint32_t LightingCache::GetPointLightCount() const
{
	return m_pointLightSSBO.size();
}

// ---------------------------------------------------------------------------
// Sun light SSBO
// ---------------------------------------------------------------------------

void LightingCache::UpdateSunLights(const std::vector<SunLightStruct>& lights)
{
	m_sunLightSSBO.Upload(lights);
	NEURUS_LOG("[LightingCache] Uploaded " << m_sunLightSSBO.size() << " sun lights");
}

void LightingCache::UpdateSunLight(const SunLightStruct& light, uint32_t index)
{
	m_sunLightSSBO.Update(light, index);
}

const GPUBuffer* LightingCache::GetSunLightSSBO() const
{
	return m_sunLightSSBO.gpuBuffer();
}

uint32_t LightingCache::GetSunLightCount() const
{
	return m_sunLightSSBO.size();
}

// ---------------------------------------------------------------------------
// Spot light SSBO
// ---------------------------------------------------------------------------

void LightingCache::UpdateSpotLights(const std::vector<SpotLightStruct>& lights)
{
	m_spotLightSSBO.Upload(lights);
	NEURUS_LOG("[LightingCache] Uploaded " << m_spotLightSSBO.size() << " spot lights");
}

void LightingCache::UpdateSpotLight(const SpotLightStruct& light, uint32_t index)
{
	m_spotLightSSBO.Update(light, index);
}

const GPUBuffer* LightingCache::GetSpotLightSSBO() const
{
	return m_spotLightSSBO.gpuBuffer();
}

uint32_t LightingCache::GetSpotLightCount() const
{
	return m_spotLightSSBO.size();
}

} // namespace neurus
