#pragma once

#include "command/ICommandExecutor.h"
#include "command/ISessionState.h"

#include <functional>

namespace neurus
{

/**
 * @brief Executes the session.status and session.close commands.
 *
 * Close policy lives here rather than in the IPC layer so it can be tested
 * against a fake ISessionState without Qt or a window.
 */
class SessionCommandExecutor final : public command::ICommandExecutor
{
public:
	/**
	 * @param state Live application state.
	 * @param requestClose Invoked once a close has been accepted.
	 */
	SessionCommandExecutor(command::ISessionState& state, std::function<void()> requestClose);

	command::CommandResult Execute(const command::CommandRequest& request) override;

	/// @return True after an accepted close; the app should shut down.
	bool CloseRequested() const;

private:
	command::CommandResult ExecuteStatus() const;
	command::CommandResult ExecuteClose(const command::CommandRequest& request);

	command::ISessionState& m_state;
	std::function<void()>   m_requestClose;
	bool                    m_closeRequested = false;
};

} // namespace neurus
