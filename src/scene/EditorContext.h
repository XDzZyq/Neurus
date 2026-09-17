/**
 * @file EditorContext.h
 * @brief Shared read-only snapshot of Editor-owned state (scene + config).
 *
 * EditorContext is the common editor state that both the Renderer and the UI
 * need each frame. It is produced once by Editor::GetContext() and embedded by
 * value into both RenderContext (render layer) and UIContext (UI layer), so the
 * Editor exposes a single source of truth instead of building each context
 * separately.
 *
 * Architecture:
 * - Lives in the Vulkan-free scene layer so both render/ and ui/ may include it.
 * - `scene` is the Editor-owned Scene upcast to its UID base; consumers cast it
 *   back to `const Scene*`.
 * - `config` stays an opaque `const void*` (a RenderConfig*) so this header does
 *   not pull the renderer's RenderConfig into the UI layer.
 */

#pragma once

#include "core/UID.h"

namespace neurus
{

struct DebugDrawList;

/**
 * @brief Editor-owned scene + render config, shared by RenderContext/UIContext.
 */
struct EditorContext
{
	/// @brief Editor-owned Scene (upcast to UID). Cast to const Scene* to use.
	const UID* scene = nullptr;

	/// @brief Opaque RenderConfig*. Cast to const RenderConfig* to read flags.
	const void* config = nullptr;

	/**
	 * @brief This frame's debug geometry, or nullptr when debug draw is off.
	 *
	 * Owned by the Editor and rebuilt every Edit(); valid only for the frame it
	 * was published in. Forward-declared rather than included so this header
	 * stays free of glm/<vector> for the UI layer — include
	 * scene/DebugDrawList.h where the contents are actually read.
	 */
	const DebugDrawList* debugDraw = nullptr;
};

} // namespace neurus
