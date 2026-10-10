/**
 * @file SceneContext.cpp
 * @brief The one place a missing RenderConfig is turned into the defaults.
 *
 * This translation unit exists only so SceneContext.h can keep RenderConfig on a
 * forward declaration while Settings() still needs the complete type. That trade
 * is the point of the file: the fallback is written once here instead of as a
 * `cfg ? cfg->x : <literal>` ternary at every reader.
 */

#include "scene/SceneContext.h"

#include "core/RenderConfig.h"

namespace neurus
{

const RenderConfig& SceneContext::Settings() const
{
	// Function-local static, so the fallback *is* RenderConfig's in-class member
	// initializers rather than a second copy of them: adding or changing a default
	// in RenderConfig.h changes this with no edit here. Const and immutable, so
	// sharing one instance across frames and threads is safe.
	static const RenderConfig kDefaults{};
	return config ? *config : kDefaults;
}

} // namespace neurus
