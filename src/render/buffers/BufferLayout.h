#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <stdexcept>
#include <vector>

namespace neurus {

/**
 * @brief Describes the vertex attribute layout for a single vertex buffer binding.
 *
 * Stores a list of vertex input attribute descriptions (location, format,
 * offset) and produces the Vulkan structures for pipeline creation.
 *
 * @note Configurable - no hardcoded vertex layouts. Callers add attributes
 *       matching their vertex data structure.
 */
class BufferLayout
{
public:
	BufferLayout() = default;

	/**
	 * @brief Add a vertex attribute description.
	 *
	 * @param location Shader input location (must be 0..maxVertexInputAttributes-1).
	 * @param format   Vulkan format for the attribute (e.g. eR32G32B32Sfloat).
	 * @param offset   Byte offset from the start of the vertex.
	 *
	 * @note The stride is automatically updated to account for this attribute.
	 */
	void AddAttribute(uint32_t location, vk::Format format, uint32_t offset);

	/**
	 * @brief Returns the Vulkan vertex input binding description.
	 *
	 * Uses binding = 0, calculated stride, and per-vertex input rate.
	 *
	 * @return VkVertexInputBindingDescription ready for pipeline creation.
	 */
	vk::VertexInputBindingDescription GetBindingDescription() const;

	/**
	 * @brief Returns a read-only reference to the attribute descriptions.
	 *
	 * @return Vector of VkVertexInputAttributeDescription, in the order added.
	 */
	const std::vector<vk::VertexInputAttributeDescription>& GetAttributeDescriptions() const;

	/**
	 * @brief Returns the total vertex stride in bytes.
	 *
	 * Computed as: last attribute offset + format size. Returns 0 when no
	 * attributes are added.
	 *
	 * @return Total byte stride of a single vertex.
	 */
	uint32_t GetStride() const;

	/**
	 * @brief Returns the byte size of a given Vulkan format.
	 *
	 * @param format Vulkan format to query.
	 * @return Size in bytes.
	 * @throws std::runtime_error for unsupported formats.
	 */
	static uint32_t GetFormatSize(vk::Format format);

	/** @brief Number of attributes currently registered. */
	size_t GetAttributeCount() const { return b_attributes.size(); }

private:
	std::vector<vk::VertexInputAttributeDescription> b_attributes;
};

} // namespace neurus
