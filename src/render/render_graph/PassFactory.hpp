/**
 * @file PassFactory.hpp
 * @brief Pass kind to live instance, and the port source for the graph compiler.
 *
 * The factory lazily caches one GPU instance per built-in scene pass. The renderer
 * compiles a document through it, so a node's `pass` string resolves to a live
 * pass. CPU port queries use the immutable built-in provider without GPU work.
 */

#pragma once

#include "asset/data/render_graph/PortSource.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neurus {

class Pass;
class GeometryPass;
class LightingPass;
class SSAOPass;
class ShadowDepthPass;
class ShadowIntensityPass;
class ComposePass;
class FXAAPass;
class SharedQueue;

/**
 * @brief Owns the built-in scene passes and answers pass-kind queries.
 *
 * Non-copyable, non-movable (owns GPU resources).
 */
class PassFactory : public IPortSource
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
	 */
	PassFactory(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice,
	             uint32_t numSets,
	             SharedQueue& graphicsQueue,
	             uint32_t queueFamilyIndex);

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

	/// @brief Returns the cached ports for a kind, or nullptr when unknown.
	const PassPorts* FindPassPorts(std::string_view passKind) const override;

	/// @brief Returns the pass's declared out-of-graph inputs.
	std::vector<std::string> ExternalInputs(std::string_view passKind) const override;

	/// @brief Returns the cached ports for a kind.
	/// @throws std::invalid_argument when the kind is unknown.
	const PassPorts& Ports(std::string_view passKind) const;

	// --- Typed accessors for the renderer's request path ---
	GeometryPass* Geometry();
	LightingPass* Lighting();
	SSAOPass* SSAO();
	ShadowDepthPass* ShadowDepth();
	ShadowIntensityPass* ShadowIntensity();
	ComposePass* Compose();
	FXAAPass* FXAA();

private:
	void Add(std::string kind, std::unique_ptr<Pass> pass);

	const vk::raii::Device& m_device;
	const vk::raii::PhysicalDevice& m_physicalDevice;
	uint32_t m_numSets;
	SharedQueue& m_graphicsQueue;
	uint32_t m_queueFamilyIndex;
	std::unordered_map<std::string, std::unique_ptr<Pass>> m_passes;
};

} // namespace neurus
