#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <vector>

namespace neurus {

// Forward declarations
class DescriptorSetLayout;
class DescriptorPool;
class DescriptorSet;

// ---------------------------------------------------------------------------
// DescriptorSetLayoutBuilder - Fluent API for building bindings vectors
// ---------------------------------------------------------------------------

/**
 * @brief Fluent builder for accumulating VkDescriptorSetLayoutBinding entries.
 *
 * Chained via the free function BuildLayout() to assemble binding vectors
 * with a readable syntax.
 */
class DescriptorSetLayoutBuilder
{
public:
	/**
	 * @brief Adds a descriptor binding to the layout.
	 *
	 * @param binding      Shader binding number.
	 * @param type         Descriptor type (uniform buffer, sampler, etc.).
	 * @param stageFlags   Shader stages that access this binding.
	 * @param count        Number of descriptors in the binding (for arrays,
	 *                     default 1).
	 * @return Reference to this builder for fluent chaining.
	 */
	DescriptorSetLayoutBuilder& AddBinding(
		uint32_t binding,
		vk::DescriptorType type,
		vk::ShaderStageFlags stageFlags,
		uint32_t count = 1);

	/**
	 * @brief Adds a descriptor binding with binding flags.
	 *
	 * @param binding      Shader binding number.
	 * @param type         Descriptor type (uniform buffer, sampler, etc.).
	 * @param stageFlags   Shader stages that access this binding.
	 * @param flags        Binding flags (e.g. PARTIALLY_BOUND).
	 * @param count        Number of descriptors in the binding (for arrays,
	 *                     default 1).
	 * @return Reference to this builder for fluent chaining.
	 */
	DescriptorSetLayoutBuilder& AddBindingWithFlags(
		uint32_t binding,
		vk::DescriptorType type,
		vk::ShaderStageFlags stageFlags,
		vk::DescriptorBindingFlags flags,
		uint32_t count = 1);

	/**
	 * @brief Finalizes and returns the accumulated binding vector.
	 * @return Vector of descriptor set layout bindings ready for
	 *         DescriptorSetLayout construction.
	 */
	std::vector<vk::DescriptorSetLayoutBinding> Build();

	/**
	 * @brief Finalizes and returns a DescriptorSetLayout directly.
	 * @param device Logical device for layout creation.
	 * @return DescriptorSetLayout wrapping the accumulated bindings.
	 */
	DescriptorSetLayout Build(const vk::raii::Device& device);

private:
	std::vector<vk::DescriptorSetLayoutBinding> desc_bindings;
	std::vector<vk::DescriptorBindingFlags> desc_bindingFlags;
};

/**
 * @brief Entry point for the fluent descriptor layout builder.
 * @return A DescriptorSetLayoutBuilder ready for chaining.
 */
DescriptorSetLayoutBuilder BuildLayout();

// ---------------------------------------------------------------------------
// DescriptorSetLayout - RAII wrapper around vk::raii::DescriptorSetLayout
// ---------------------------------------------------------------------------

/**
 * @brief Owns a vk::raii::DescriptorSetLayout created from a vector of bindings.
 *
 * Non-copyable, movable.
 */
class DescriptorSetLayout
{
public:
	/**
	 * @brief Default-constructs a null descriptor set layout.
	 *
	 * The layout is nullptr until assigned via the parameterised constructor
	 * or move assignment.
	 */
	DescriptorSetLayout() = default;

	/**
	 * @brief Creates the Vulkan descriptor set layout from the given bindings.
	 *
	 * @param device    Logical device (must outlive this layout).
	 * @param bindings  Vector of VkDescriptorSetLayoutBinding entries.
	 */
	DescriptorSetLayout(
		const vk::raii::Device& device,
		const std::vector<vk::DescriptorSetLayoutBinding>& bindings);

	/**
	 * @brief Creates the Vulkan descriptor set layout with per-binding flags.
	 *
	 * The bindingFlags vector must have the same size as bindings. Each
	 * entry corresponds to the binding at the same index. Bindings with
	 * flags=0 behave the same as the flags-less constructor.
	 *
	 * @param device        Logical device (must outlive this layout).
	 * @param bindings      Vector of VkDescriptorSetLayoutBinding entries.
	 * @param bindingFlags  Per-binding flags (e.g. PARTIALLY_BOUND).
	 */
	DescriptorSetLayout(
		const vk::raii::Device& device,
		const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
		const std::vector<vk::DescriptorBindingFlags>& bindingFlags);

	DescriptorSetLayout(const DescriptorSetLayout&) = delete;
	DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;
	DescriptorSetLayout(DescriptorSetLayout&&) noexcept = default;
	DescriptorSetLayout& operator=(DescriptorSetLayout&&) noexcept = default;

	/** @brief Underlying vk::raii::DescriptorSetLayout handle. */
	const vk::raii::DescriptorSetLayout& layout() const
	{
		return desc_layout;
	}

	/** @brief The bindings used to create this layout (for introspection). */
	const std::vector<vk::DescriptorSetLayoutBinding>& bindings() const
	{
		return desc_bindings;
	}

private:
	vk::raii::DescriptorSetLayout desc_layout = nullptr;
	std::vector<vk::DescriptorSetLayoutBinding> desc_bindings;
};

// ---------------------------------------------------------------------------
// DescriptorSet - A single allocated descriptor set with write helpers
// ---------------------------------------------------------------------------

/**
 * @brief Holds one vk::raii::DescriptorSet allocated from a DescriptorPool.
 *
 * Provides convenience WriteBuffer / WriteImage methods that build
 * VkWriteDescriptorSet and call vkUpdateDescriptorSets immediately.
 *
 * Move-only.
 *
 * @note The underlying vk::raii::DescriptorSet holds a reference to its
 *       parent pool. The pool must outlive all allocated DescriptorSets,
 *       or be created with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT.
 */
class DescriptorSet
{
public:
	/**
	 * @brief Constructs from an already-allocated vk::raii::DescriptorSet.
	 *
	 * @param set    An allocated descriptor set (moved in).
	 * @param device Logical device for updateDescriptorSets calls.
	 */
	DescriptorSet(vk::raii::DescriptorSet&& set,
	              const vk::raii::Device* device);

	DescriptorSet(const DescriptorSet&) = delete;
	DescriptorSet& operator=(const DescriptorSet&) = delete;
	DescriptorSet(DescriptorSet&&) noexcept = default;
	DescriptorSet& operator=(DescriptorSet&&) noexcept = default;

	/**
	 * @brief Writes a buffer descriptor to the given binding.
	 *
	 * Calls vkUpdateDescriptorSets immediately with the provided info.
	 *
	 * @param binding    Shader binding number.
	 * @param bufferInfo Pre-filled VkDescriptorBufferInfo.
	 * @param type       Descriptor type (default: eUniformBuffer).
	 */
	void WriteBuffer(uint32_t binding,
	                 const vk::DescriptorBufferInfo& bufferInfo,
	                 vk::DescriptorType type =
	                     vk::DescriptorType::eUniformBuffer);

	/**
	 * @brief Writes an image descriptor to the given binding.
	 *
	 * Calls vkUpdateDescriptorSets immediately with the provided info.
	 *
	 * @param binding    Shader binding number.
	 * @param imageInfo  Pre-filled VkDescriptorImageInfo.
	 * @param type       Descriptor type (default: eCombinedImageSampler).
	 */
	void WriteImage(uint32_t binding,
	                const vk::DescriptorImageInfo& imageInfo,
	                vk::DescriptorType type =
	                    vk::DescriptorType::eCombinedImageSampler);

	/** @brief Raw VkDescriptorSet handle for binding in command recording. */
	vk::DescriptorSet handle() const { return *desc_set; }

#ifdef _DEBUG
	/**
	 * @brief Sets a debug name on the underlying VkDescriptorSet for RenderDoc / validation.
	 * @param name Debug name string (must be null-terminated, not retained).
	 */
	void SetDebugName(const char* name);
#endif

private:
	vk::raii::DescriptorSet desc_set = nullptr;
	const vk::raii::Device* desc_device;
};

// ---------------------------------------------------------------------------
// DescriptorPool - RAII wrapper around vk::raii::DescriptorPool
// ---------------------------------------------------------------------------

/**
 * @brief Owns a vk::raii::DescriptorPool and allocates DescriptorSet objects.
 *
 * Created with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT so
 * individual DescriptorSet objects can be freed safely before the pool.
 * Use CalculatePoolSizes() to derive pool sizes from the layouts to allocate.
 *
 * Non-copyable, movable.
 */
class DescriptorPool
{
public:
	/**
	 * @brief Default-constructs a null descriptor pool.
	 *
	 * The pool is nullptr until assigned via the parameterised constructor
	 * or move assignment.
	 */
	DescriptorPool() = default;

	/**
	 * @brief Creates a descriptor pool with the given capacity.
	 *
	 * @param device     Logical device (must outlive this pool).
	 * @param maxSets    Maximum number of descriptor sets that can be
	 *                   allocated from this pool.
	 * @param poolSizes  Per-type descriptor counts the pool must support.
	 */
	DescriptorPool(const vk::raii::Device& device,
	               uint32_t maxSets,
	               const std::vector<vk::DescriptorPoolSize>& poolSizes);

	DescriptorPool(const DescriptorPool&) = delete;
	DescriptorPool& operator=(const DescriptorPool&) = delete;
	DescriptorPool(DescriptorPool&&) noexcept = default;
	DescriptorPool& operator=(DescriptorPool&&) noexcept = default;

	/**
	 * @brief Allocates one or more descriptor sets with the given layout.
	 *
	 * @param layout Descriptor set layout to allocate against.
	 * @param count  Number of sets to allocate (default 1).
	 * @return Vector of allocated DescriptorSet objects.
	 *
	 * @throws std::runtime_error if the pool is exhausted.
	 */
	std::vector<DescriptorSet> Allocate(const DescriptorSetLayout& layout,
	                                    uint32_t count = 1);

	/**
	 * @brief Auto-calculates pool sizes from a set of layouts.
	 *
	 * Iterates all bindings in the provided layouts, accumulating per-type
	 * counts, then multiplies by the given multiplier.
	 *
	 * @param layouts     Vector of layout pointers to analyze.
	 * @param multiplier  Factor applied to total counts (use the number of
	 *                    swapchain images or total sets you plan to allocate).
	 * @return Pool sizes suitable for DescriptorPool construction.
	 */
	static std::vector<vk::DescriptorPoolSize> CalculatePoolSizes(
		const std::vector<const DescriptorSetLayout*>& layouts,
		uint32_t multiplier = 1);

	/** @brief Underlying vk::raii::DescriptorPool handle. */
	const vk::raii::DescriptorPool& pool() const { return desc_pool; }

private:
	vk::raii::DescriptorPool desc_pool = nullptr;
	const vk::raii::Device* desc_device;
};

} // namespace neurus
