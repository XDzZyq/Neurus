/**
 * @file ShaderGPU.hpp
 * @brief Uploaded shader modules and their owned reflection metadata.
 */

#pragma once

#include "render_api/RenderRequests.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <unordered_map>

namespace neurus
{

/**
 * @brief Immutable GPU shader resource created by UploadManager.
 *
 * Owns modules and uploaded stage metadata without retaining a live CPU asset.
 * Passes borrow module handles while retaining the shared resource itself.
 */
class ShaderGPU
{
public:
	/**
	 * @brief Creates a module per uploaded stage.
	 * @param device Device owning the resulting modules; must outlive this resource.
	 * @param request Validated stage bytecode and reflection metadata.
	 * @throws std::invalid_argument for empty stages.
	 */
	ShaderGPU(const vk::raii::Device& device, const UploadShaderRequest& request);

	ShaderGPU(const ShaderGPU&) = delete;
	ShaderGPU& operator=(const ShaderGPU&) = delete;
	ShaderGPU(ShaderGPU&&) = delete;
	ShaderGPU& operator=(ShaderGPU&&) = delete;

	/** @brief Logical CPU shader identity used by the GPU cache. */
	int GetObjectID() const { return m_id; }

	/** @brief Version captured by the upload request. */
	int GetVersion() const { return m_version; }

	/** @brief Primary shader stage identifying graphics or compute behavior. */
	ShaderType GetType() const { return m_type; }

	/** @brief True when a stage has uploaded metadata and a module. */
	bool HasStage(ShaderType stage) const { return m_stages.contains(stage); }

	/** @brief Returns owned stage metadata, or null when that stage is absent. */
	const ShaderStageUpload* FindStage(ShaderType stage) const;

	/** @brief Returns owned stage metadata; throws std::out_of_range if absent. */
	const ShaderStageUpload& GetStage(ShaderType stage) const { return m_stages.at(stage); }

	/** @brief Borrows a module handle; throws std::out_of_range if absent. */
	vk::ShaderModule Module(ShaderType stage) const { return *m_modules.at(stage); }

private:
	int m_id;
	int m_version;
	ShaderType m_type;
	std::unordered_map<ShaderType, ShaderStageUpload> m_stages;
	std::unordered_map<ShaderType, vk::raii::ShaderModule> m_modules;
};

} // namespace neurus
