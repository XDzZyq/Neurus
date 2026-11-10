#pragma once

#include "command/ICommandExecutor.h"
#include "command/IEditorCommands.h"
#include "command/IScriptCommands.h"
#include "command/ISessionState.h"

#include <functional>

namespace neurus
{

/**
 * @brief Executes session, project, scene and object commands.
 *
 * Session and project policy lives here rather than in the IPC layer so it can
 * be tested against a fake ISessionState without Qt or a window. Scene and
 * object commands are delegated to the editor, which owns the scene and the
 * typed event queue.
 */
class SessionCommandExecutor final : public command::ICommandExecutor
{
public:
	/**
	 * @param state Live application state.
	 * @param requestClose Invoked once a close has been accepted.
	 * @param editor Scene/object sub-executor; null when the session has no
	 *               editor command surface.
	 * @param script Script sub-executor; null when scripting is unavailable.
	 */
	SessionCommandExecutor(command::ISessionState& state,
	                       std::function<void()> requestClose,
	                       command::IEditorCommands* editor = nullptr,
	                       command::IScriptCommands* script = nullptr);

	command::CommandResult Execute(const command::CommandRequest& request) override;

	/// @return True after an accepted close; the app should shut down.
	bool CloseRequested() const;

private:
	command::CommandResult ExecuteStatus() const;
	command::CommandResult ExecuteClose(const command::CommandRequest& request);

	command::CommandResult ExecuteProject(const command::CommandRequest& request);
	command::CommandResult ExecuteProjectInfo() const;
	command::CommandResult ExecuteProjectNew(const command::CommandRequest& request);
	command::CommandResult ExecuteProjectOpen(const command::CommandRequest& request);
	command::CommandResult ExecuteProjectSave(const command::CommandRequest& request);

	command::ISessionState& m_state;
	std::function<void()>   m_requestClose;
	command::IEditorCommands* m_editor = nullptr;
	command::IScriptCommands* m_script = nullptr;
	bool                    m_closeRequested = false;
};

} // namespace neurus
