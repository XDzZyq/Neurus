/**
 * @file PassFactory.hpp
 * @brief Constructs and reuses runtime GPU passes by kind and shader selection.
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace neurus {

class Pass;
class ShaderGPU;
class RenderCache;
class GeometryPass;
class SharedQueue;

/**
 * @brief Reuses node-local pass state while all instances share uploaded shader modules.
 *
 * Non-copyable, non-movable (owns GPU resources).
 */
class PassFactory
{
public:
	/**
	 * @brief Binds the device services; passes are created on first use.
	 *
	 * @param device            Logical device (borrowed).
	 * @param physicalDevice    Physical device (borrowed).
	 * @param numSets           Descriptor sets per pass, one per in-flight frame.
	 * @param graphicsQueue     Serialised graphics queue (borrowed).
	 * @param queueFamilyIndex  Queue family for staging work.
	 * @param cache             Uploaded shader resources (borrowed).
	 */
	PassFactory(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice,
	             uint32_t numSets,
	             SharedQueue& graphicsQueue,
	             uint32_t queueFamilyIndex,
	             RenderCache& cache);

	~PassFactory();

	PassFactory(const PassFactory&)            = delete;
	PassFactory& operator=(const PassFactory&) = delete;
	PassFactory(PassFactory&&)                 = delete;
	PassFactory& operator=(PassFactory&&)      = delete;

	/**
	 * @brief Returns the pass for a kind, or nullptr when the kind is unknown.
	 * @param kind  Pass kind the document names ("GeometryPass", ...).
	 */
	Pass* Find(std::string_view kind);

	/** @brief Resolves an uploaded shader into node-local GPU pass state. */
	std::shared_ptr<Pass> Acquire(std::string_view kind, std::string_view node,
	                              std::optional<int> shaderId);

	/** @brief Resolves an uploaded shader ID, or a built-in default when the ID is absent. */
	std::shared_ptr<const ShaderGPU> ShaderFor(std::string_view kind, std::optional<int> shaderId = std::nullopt);

	/// @brief Geometry pass used to warm mesh pipelines.
	GeometryPass* Geometry();

private:
	void Add(std::string kind, std::unique_ptr<Pass> pass);
	std::unique_ptr<Pass> Create(std::string_view kind, std::shared_ptr<const ShaderGPU> shader);

	RenderCache& m_cache;
	const vk::raii::Device& m_device;
	const vk::raii::PhysicalDevice& m_physicalDevice;
	uint32_t m_numSets;
	SharedQueue& m_graphicsQueue;
	uint32_t m_queueFamilyIndex;
	std::unordered_map<std::string, std::unique_ptr<Pass>> m_passes;
	std::unordered_map<std::string, std::weak_ptr<Pass>> m_nodePasses;
};

} // namespace neurus
