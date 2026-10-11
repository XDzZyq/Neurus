/**
 * @file Pass.cpp
 * @brief Pass type query helpers implementation.
 */

#include "passes/Pass.h"

#include <cassert>
#include <stdexcept>

namespace neurus {

// ============================================================================
// Static helpers - pass configuration
// ============================================================================

vk::AttachmentLoadOp Pass::ColorLoadOpFor(PassType passType)
{
	switch (passType)
	{
	case PassType::G_BUFFER:
	case PassType::LIGHTING:
		return vk::AttachmentLoadOp::eClear;
	case PassType::SHADOW:
	case PassType::COMPOSITE:
	case PassType::POST_FX:
		return vk::AttachmentLoadOp::eDontCare;
	}
	return vk::AttachmentLoadOp::eDontCare;
}

vk::AttachmentStoreOp Pass::ColorStoreOpFor(PassType passType)
{
	switch (passType)
	{
	case PassType::G_BUFFER:
	case PassType::LIGHTING:
	case PassType::COMPOSITE:
	case PassType::POST_FX:
		return vk::AttachmentStoreOp::eStore;
	case PassType::SHADOW:
		return vk::AttachmentStoreOp::eDontCare;
	}
	return vk::AttachmentStoreOp::eStore;
}

vk::AttachmentLoadOp Pass::DepthLoadOpFor(PassType passType)
{
	switch (passType)
	{
	case PassType::G_BUFFER:
	case PassType::SHADOW:
		return vk::AttachmentLoadOp::eClear;
	case PassType::LIGHTING:
	case PassType::COMPOSITE:
	case PassType::POST_FX:
		return vk::AttachmentLoadOp::eDontCare;
	}
	return vk::AttachmentLoadOp::eDontCare;
}

vk::AttachmentStoreOp Pass::DepthStoreOpFor(PassType passType)
{
	switch (passType)
	{
	case PassType::G_BUFFER:
	case PassType::SHADOW:
		return vk::AttachmentStoreOp::eStore;
	case PassType::LIGHTING:
	case PassType::COMPOSITE:
	case PassType::POST_FX:
		return vk::AttachmentStoreOp::eDontCare;
	}
	return vk::AttachmentStoreOp::eDontCare;
}

// ============================================================================
// Static queries
// ============================================================================

uint32_t Pass::ColorAttachmentCount(PassType passType)
{
	switch (passType)
	{
	case PassType::G_BUFFER:   return 5;
	case PassType::LIGHTING:   return 1;
	case PassType::SHADOW:     return 0;
	case PassType::COMPOSITE:  return 1;
	case PassType::POST_FX:    return 1;
	}
	return 0;
}

bool Pass::HasDepth(PassType passType)
{
	switch (passType)
	{
	case PassType::G_BUFFER:   return true;
	case PassType::SHADOW:     return true;
	case PassType::LIGHTING:
	case PassType::COMPOSITE:
	case PassType::POST_FX:    return false;
	}
	return false;
}

std::vector<vk::ClearValue> Pass::PresetClearValues(PassType passType)
{
	std::vector<vk::ClearValue> result;
	const uint32_t colorCount = ColorAttachmentCount(passType);
	const bool hasDepth = HasDepth(passType);

	const vk::ClearValue blackColor(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}));
	result.insert(result.end(), colorCount, blackColor);

	if (hasDepth)
	{
		const vk::ClearValue depthClear(vk::ClearDepthStencilValue(1.0f, 0));
		result.push_back(depthClear);
	}

	return result;
}

} // namespace neurus
