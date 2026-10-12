/**
 * @file ScriptContext.h
 * @brief Read-only view of the scene and render config handed to scripts.
 *
 * `ScriptContext` is a plain aggregate of non-owning pointers plus a snapshot of
 * selection UIDs, so a later task can expose scene/config queries to Python
 * without giving the Script layer write access. It deliberately forward-declares
 * `Scene` and `RenderConfig` (never includes them) so that including this header
 * does not pull editor/renderer state into a translation unit.
 *
 * The builder is a free function in the Script layer, not a member of `Editor`:
 * a member would force `editor -> script` and violate the unidirectional layer
 * rule (the Script layer sits ON the Editor layer).
 */

#pragma once

#include <vector>

namespace neurus
{

class Scene;
class RenderConfig;

/**
 * @brief Non-owning, read-only snapshot of scene/config state for scripts.
 *
 * All pointers are borrowed; the caller guarantees they outlive the context.
 */
struct ScriptContext
{
	const Scene* scene = nullptr;          ///< Borrowed scene being edited.
	const RenderConfig* config = nullptr;  ///< Borrowed render configuration.
	int active_camera_id = -1;             ///< `Scene::ActiveCameraID()`, or -1 when unset.
	std::vector<int> selection;            ///< Selected object UIDs, order preserved.
};

/**
 * @brief Fills @p out from a scene and render config.
 *
 * Sets @p out.scene / @p out.config to the borrowed addresses, snapshots
 * `Scene::ActiveCameraID()` into `active_camera_id`, and copies the selected
 * object UIDs into `selection` (via `SnapshotSelectionUids`).
 *
 * @param scene Scene to expose; must outlive @p out.
 * @param config Render config to expose; must outlive @p out.
 * @param out Context to fill.
 */
void BuildScriptContext(const Scene& scene, const RenderConfig& config, ScriptContext& out);

}  // namespace neurus
