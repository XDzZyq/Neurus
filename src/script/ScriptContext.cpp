/**
 * @file ScriptContext.cpp
 * @brief Definition of the Script-layer `BuildScriptContext` free function.
 */

#include "script/ScriptContext.h"

#include "core/RenderConfig.h"
#include "scene/Scene.h"

namespace neurus
{

void BuildScriptContext(const Scene& scene, const RenderConfig& config, ScriptContext& out)
{
	out.scene = &scene;
	out.config = &config;
	out.active_camera_id = scene.ActiveCameraID();

	// The active UID snapshot is intentionally discarded here; only the ordered
	// selected UIDs are part of the script-facing read path.
	int activeUid = 0;
	SnapshotSelectionUids(scene.selections, out.selection, activeUid);
}

}  // namespace neurus
