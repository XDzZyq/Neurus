#pragma once

#include "command/IEditorCommands.h"

namespace neurus
{

class Editor;

/**
 * @brief Executes scene and object commands against the live editor.
 *
 * Reads go straight to the scene and the resource pool. Writes emit the same
 * typed events the panels emit, so controllers apply them synchronously and the
 * undo history sees them exactly like a UI edit.
 */
class EditorCommandAdapter final : public command::IEditorCommands
{
public:
	/**
	 * @param editor Live editor; must outlive this adapter.
	 */
	explicit EditorCommandAdapter(Editor& editor);

	command::CommandResult Execute(const command::CommandRequest& request) override;

private:
	command::CommandResult ExecuteSceneList(const command::Json& args);
	command::CommandResult ExecuteObjectGet(const command::Json& args);
	command::CommandResult ExecuteObjectSet(const command::Json& args);
	command::CommandResult ExecuteObjectLoad(const command::Json& args);
	command::CommandResult ExecuteObjectDelete(const command::Json& args);
	command::CommandResult ExecuteRenderGet();
	command::CommandResult ExecuteRenderSet(const command::Json& args);
	command::CommandResult ExecuteHistoryGet();
	command::CommandResult ExecuteHistoryUndo();
	command::CommandResult ExecuteHistoryRedo();

	Editor& m_editor;
};

} // namespace neurus
