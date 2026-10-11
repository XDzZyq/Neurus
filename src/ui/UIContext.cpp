/**
 * @file UIContext.cpp
 * @brief UIContext implementation - the settings fallback and scene object enumeration.
 */

#include "UIContext.h"

#include "core/RenderConfig.h"
#include "scene/Scene.h"

namespace neurus
{

// =========================================================================
// Settings - the one place a panel's missing RenderConfig is answered
// =========================================================================

const RenderConfig& UIContext::Settings() const
{
	// DefaultRenderConfig() *is* RenderConfig's in-class member initializers,
	// so a panel cannot privately disagree with a render pass on a default.
	return config ? *config : DefaultRenderConfig();
}

// =========================================================================
// GetObjectIDs - collect all scene object UIDs from the published scene
// =========================================================================

std::vector<int> UIContext::GetObjectIDs() const
{
	std::vector<int> result;
	if (!HasScene())
	{
		return result;
	}

	const Scene* s = scene;
	result.reserve(s->obj_list.size());
	for (const auto& [id, obj] : s->obj_list)
	{
		(void)obj;
		result.push_back(id);
	}
	return result;
}

} // namespace neurus
