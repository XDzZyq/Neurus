/**
 * @file UIContext.cpp
 * @brief UIContext implementation — scene object enumeration.
 */

#include "UIContext.h"

#include "scene/Scene.h"

namespace neurus
{

// =========================================================================
// GetObjectIDs — collect all scene object UIDs from the published scene
// =========================================================================

std::vector<int> UIContext::GetObjectIDs() const
{
	std::vector<int> result;
	if (!snapshot.HasScene())
	{
		return result;
	}

	const Scene* s = snapshot.scene;
	result.reserve(s->obj_list.size());
	for (const auto& [id, obj] : s->obj_list)
	{
		(void)obj;
		result.push_back(id);
	}
	return result;
}

} // namespace neurus
