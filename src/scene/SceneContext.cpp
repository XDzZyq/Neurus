/**
 * @file SceneContext.cpp
 * @brief The one place a missing RenderConfig is turned into the defaults.
 *
 * This translation unit exists only so SceneContext.h can keep RenderConfig on
 * a forward declaration while Settings() still needs the complete type.
 */

#include "scene/SceneContext.h"

#include "core/RenderConfig.h"

namespace neurus
{

const RenderConfig& SceneContext::Settings() const
{
	// Function-local static: the fallback is a default-constructed RenderConfig,
	// so its in-class member initializers stay the only definition of the
	// defaults. Const, so sharing one instance across frames and threads is safe.
	static const RenderConfig kDefaults{};
	return config ? *config : kDefaults;
}

} // namespace neurus
