/**
 * @file ShaderGPU.hpp
 * @brief Uploaded shader modules and their owned reflection metadata.
 */

#pragma once

#include "render_api/RenderRequests.hpp"
#include "asset/data/render_graph/PassPorts.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <span>
#include <string>
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
	 * @brief Creates modules and retains the CPU-derived image ports.
	 * @param device Device owning the resulting modules; must outlive this resource.
	 * @param request Validated stage bytecode and reflection metadata.
	 * @throws std::invalid_argument for empty stages or conflicting port declarations.
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

	/** @brief Image port interface carried by the upload request. */
	const PassPorts& Ports() const { return m_ports; }

private:
	int m_id;
	int m_version;
	ShaderType m_type;
	std::unordered_map<ShaderType, ShaderStageUpload> m_stages;
	std::unordered_map<ShaderType, vk::raii::ShaderModule> m_modules;
	PassPorts m_ports;
};

/**
 * @brief Collects uploaded ports and the kind's declared outputs for a standalone pass.
 * @param kind Pass kind used in the built-in catalog, or a label for a legacy pass.
 * @param shaders Uploaded shader resources; null entries are skipped.
 * @return Merged ports and diagnostics for conflicting declarations.
 */
PassPorts CollectUploadedPassPorts(std::string kind, std::span<const ShaderGPU* const> shaders);

} // namespace neurus
