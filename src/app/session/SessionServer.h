#pragma once

#include "command/ISessionState.h"
#include "ipc/SessionRegistry.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace neurus::ipc
{
class IpcServer;
}

namespace neurus::command
{
class IEditorCommands;
class IScriptCommands;
class ISnapshotCommands;
}

namespace neurus
{

class SessionCommandExecutor;

/**
 * @brief Hosts the loopback command server for one named background session.
 *
 * Owns the token, the registry entry and the server. The server is only
 * created when the application was launched with `--session`.
 */
class SessionServer
{
public:
	/**
	 * @param state Live application state used by the command executor.
	 * @param appVersion Version reported by the hello handshake and status.
	 * @param editor Scene/object sub-executor; may be null in a headless app.
	 * @param script Script sub-executor; may be null when scripting is off.
	 * @param snapshot Snapshot sub-executor; may be null in a headless app.
	 */
	SessionServer(command::ISessionState& state, std::string appVersion,
	              command::IEditorCommands* editor = nullptr,
	              command::IScriptCommands* script = nullptr,
	              command::ISnapshotCommands* snapshot = nullptr);
	~SessionServer();

	SessionServer(const SessionServer&) = delete;
	SessionServer& operator=(const SessionServer&) = delete;

	/**
	 * @brief Binds an ephemeral loopback port and publishes the registry entry.
	 * @param session Session name from `--session`.
	 * @param project Project path reported by status.
	 * @param error Receives a reason on failure.
	 */
	bool Start(const std::string& session, const std::string& project, std::string& error);

	/// Stops the server and removes the registry entry.
	void Stop();

	/// Executes pending commands on the calling (main) thread.
	void DrainOnMainThread();

	/// Waits for queued responses to be written.
	bool WaitForWrites(std::chrono::milliseconds timeout);

	/// @return True after an accepted session.close.
	bool CloseRequested() const;

	/// @return The bound loopback port.
	std::uint16_t Port() const;

private:
	command::ISessionState&                  m_state;
	std::string                              m_appVersion;
	command::IEditorCommands*                m_editorCommands = nullptr;
	command::IScriptCommands*                m_scriptCommands = nullptr;
	command::ISnapshotCommands*              m_snapshotCommands = nullptr;
	ipc::SessionRegistry                     m_registry;
	std::string                              m_session;
	std::string                              m_token;
	std::unique_ptr<SessionCommandExecutor>  m_executor;
	std::unique_ptr<ipc::IpcServer>          m_server;
	bool                                     m_started = false;
};

} // namespace neurus
