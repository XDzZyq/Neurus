#pragma once

#include <string_view>

namespace neurus::command
{

// --- Session lifecycle ---
inline constexpr const char* kSessionStatus = "session.status";
inline constexpr const char* kSessionClose = "session.close";
inline constexpr const char* kSessionHello = "session.hello";

// --- Project document ---
inline constexpr const char* kProjectInfo = "project.info";
inline constexpr const char* kProjectNew = "project.new";
inline constexpr const char* kProjectOpen = "project.open";
inline constexpr const char* kProjectSave = "project.save";

// --- Scene ---
inline constexpr const char* kSceneList = "scene.list";
inline constexpr const char* kSceneInfo = "scene.info";

// --- Objects ---
inline constexpr const char* kObjectGet = "object.get";
inline constexpr const char* kObjectSet = "object.set";
inline constexpr const char* kObjectLoad = "object.load";
inline constexpr const char* kObjectAdd = "object.add";
inline constexpr const char* kObjectDuplicate = "object.duplicate";
inline constexpr const char* kObjectDelete = "object.delete";

// --- Render settings ---
inline constexpr const char* kRenderGet = "render.get";
inline constexpr const char* kRenderSet = "render.set";

// --- Undo history ---
inline constexpr const char* kHistoryGet = "history.get";
inline constexpr const char* kHistoryUndo = "history.undo";
inline constexpr const char* kHistoryRedo = "history.redo";

// --- Scripting ---
inline constexpr const char* kScriptRun = "script.run";

/**
 * @brief True for commands implemented by the editor-side adapter.
 *
 * The command core routes by family prefix so a new editor command only needs
 * an entry here and in the catalog, not another branch in the session executor.
 */
inline bool IsEditorCommand(std::string_view name)
{
	return name.rfind("scene.", 0) == 0 || name.rfind("object.", 0) == 0
		|| name.rfind("render.", 0) == 0 || name.rfind("history.", 0) == 0;
}

} // namespace neurus::command
