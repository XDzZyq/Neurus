/**
 * @file CameraGPU.cpp
 * @brief Shared camera UBO implementation.
 */

#include "CameraGPU.h"

namespace neurus {

CameraGPU::CameraGPU(const vk::raii::Device& device,
                     const vk::raii::PhysicalDevice& physicalDevice)
	: m_ubo(device, physicalDevice, "CameraUBO")
{
}

void CameraGPU::Update(const glm::mat4& proj, const glm::mat4& view)
{
	m_proj = proj;
	m_data.viewProj = proj * view;
	m_data.view = view;

	// Host-visible and coherent: a memcpy, no staging copy and no barrier. Queue
	// submission makes the write visible to the shaders that read it.
	m_ubo.Upload(m_data);
	m_valid = true;
}

} // namespace neurus
