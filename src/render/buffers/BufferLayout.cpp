#include "BufferLayout.h"

namespace neurus {

// ---------------------------------------------------------------------------
// AddAttribute
// ---------------------------------------------------------------------------

void BufferLayout::AddAttribute(uint32_t location, vk::Format format, uint32_t offset)
{
	vk::VertexInputAttributeDescription desc;
	desc.location = location;
	desc.binding = 0;
	desc.format = format;
	desc.offset = offset;

	b_attributes.push_back(desc);
}

// ---------------------------------------------------------------------------
// GetBindingDescription
// ---------------------------------------------------------------------------

vk::VertexInputBindingDescription BufferLayout::GetBindingDescription() const
{
	vk::VertexInputBindingDescription bindingDesc;
	bindingDesc.binding = 0;
	bindingDesc.stride = GetStride();
	bindingDesc.inputRate = vk::VertexInputRate::eVertex;
	return bindingDesc;
}

// ---------------------------------------------------------------------------
// GetAttributeDescriptions
// ---------------------------------------------------------------------------

const std::vector<vk::VertexInputAttributeDescription>&
BufferLayout::GetAttributeDescriptions() const
{
	return b_attributes;
}

// ---------------------------------------------------------------------------
// GetStride
// ---------------------------------------------------------------------------

uint32_t BufferLayout::GetStride() const
{
	if (b_attributes.empty())
	{
		return 0;
	}

	const auto& last = b_attributes.back();
	return last.offset + GetFormatSize(last.format);
}

// ---------------------------------------------------------------------------
// GetFormatSize
// ---------------------------------------------------------------------------

uint32_t BufferLayout::GetFormatSize(vk::Format format)
{
	switch (format)
	{
	// --- 4-byte formats ---
	case vk::Format::eR32Sfloat:
	case vk::Format::eR32Sint:
	case vk::Format::eR32Uint:
	case vk::Format::eR8G8B8A8Unorm:
	case vk::Format::eR8G8B8A8Snorm:
	case vk::Format::eR8G8B8A8Uscaled:
	case vk::Format::eR8G8B8A8Sscaled:
	case vk::Format::eR8G8B8A8Uint:
	case vk::Format::eR8G8B8A8Sint:
	case vk::Format::eR8G8B8A8Srgb:
	case vk::Format::eB8G8R8A8Unorm:
	case vk::Format::eB8G8R8A8Snorm:
	case vk::Format::eB8G8R8A8Uscaled:
	case vk::Format::eB8G8R8A8Sscaled:
	case vk::Format::eB8G8R8A8Uint:
	case vk::Format::eB8G8R8A8Sint:
	case vk::Format::eB8G8R8A8Srgb:
		return 4;

	// --- 8-byte formats ---
	case vk::Format::eR32G32Sfloat:
	case vk::Format::eR32G32Sint:
	case vk::Format::eR32G32Uint:
	case vk::Format::eR16G16B16A16Sfloat:
	case vk::Format::eR16G16B16A16Unorm:
	case vk::Format::eR16G16B16A16Snorm:
	case vk::Format::eR16G16B16A16Uscaled:
	case vk::Format::eR16G16B16A16Sscaled:
	case vk::Format::eR16G16B16A16Uint:
	case vk::Format::eR16G16B16A16Sint:
		return 8;

	// --- 12-byte formats ---
	case vk::Format::eR32G32B32Sfloat:
	case vk::Format::eR32G32B32Sint:
	case vk::Format::eR32G32B32Uint:
		return 12;

	// --- 16-byte formats ---
	case vk::Format::eR32G32B32A32Sfloat:
	case vk::Format::eR32G32B32A32Sint:
	case vk::Format::eR32G32B32A32Uint:
		return 16;

	// --- 2-byte formats ---
	case vk::Format::eR16Sfloat:
	case vk::Format::eR16Unorm:
	case vk::Format::eR16Snorm:
	case vk::Format::eR16Uscaled:
	case vk::Format::eR16Sscaled:
	case vk::Format::eR16Uint:
	case vk::Format::eR16Sint:
		return 2;

	// --- 2×2 byte formats ---
	case vk::Format::eR16G16Sfloat:
	case vk::Format::eR16G16Unorm:
	case vk::Format::eR16G16Snorm:
	case vk::Format::eR16G16Uscaled:
	case vk::Format::eR16G16Sscaled:
	case vk::Format::eR16G16Uint:
	case vk::Format::eR16G16Sint:
		return 4;

	default:
		throw std::runtime_error("BufferLayout::GetFormatSize: unsupported vertex format.");
	}
}

} // namespace neurus
