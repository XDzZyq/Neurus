#include "app/session/SessionServer.h"

#include "app/session/SessionCommandExecutor.h"
#include "command/CommandDescriptor.h"
#include "command/CommandJson.h"
#include "ipc/IpcServer.h"
#include "ipc/RandomToken.h"
#include "ipc/SessionRegistry.h"

#include <chrono>
#include <ctime>
#include <utility>

namespace neurus
{
namespace
{

std::string UtcTimestamp()
{
	const std::time_t now = std::chrono::system_clock::to_time_t(
		std::chrono::system_clock::now());
	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char buffer[32]{};
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

} // namespace

SessionServer::SessionServer(command::ISessionState& state, std::string appVersion,
                             command::IEditorCommands* editor,
                             command::IScriptCommands* script,
                             command::ISnapshotCommands* snapshot)
	: m_state(state)
	, m_appVersion(std::move(appVersion))
	, m_editorCommands(editor)
	, m_scriptCommands(script)
	, m_snapshotCommands(snapshot)
	, m_registry(ipc::ResolveSessionsDirectory())
{
}

SessionServer::~SessionServer()
{
	Stop();
}

bool SessionServer::Start(const std::string& session, const std::string& project, std::string& error)
{
	if (!ipc::IsValidSessionName(session))
	{
		error = "Invalid session name.";
		return false;
	}

	m_session = session;
	m_token = ipc::GenerateToken();
	if (m_token.empty())
	{
		error = "Cannot generate a session token.";
		return false;
	}

	m_executor = std::make_unique<SessionCommandExecutor>(
		m_state, [] {}, m_editorCommands, m_scriptCommands, m_snapshotCommands);
	m_server = std::make_unique<ipc::IpcServer>(
		ipc::IpcServerOptions{m_token, m_executor.get(), command::MakeCommandCatalog(), m_appVersion});

	if (!m_server->Start(0, error))
	{
		m_server.reset();
		m_executor.reset();
		return false;
	}

	ipc::SessionRecord record;
	record.session = m_session;
	record.pid = m_state.Pid();
	record.port = m_server->Port();
	record.token = m_token;
	record.project = project;
	record.appVersion = m_appVersion;
	record.protocolVersion = command::kProtocolVersion;
	record.startedAt = UtcTimestamp();

	if (!m_registry.Write(record, error))
	{
		m_server->Stop();
		m_server.reset();
		m_executor.reset();
		return false;
	}

	m_started = true;
	return true;
}

void SessionServer::Stop()
{
	if (m_server)
		m_server->Stop();
	if (m_started)
		m_registry.Remove(m_session);
	m_started = false;
	m_server.reset();
	m_executor.reset();
}

void SessionServer::DrainOnMainThread()
{
	if (m_server)
		m_server->DrainOnMainThread();
}

bool SessionServer::WaitForWrites(std::chrono::milliseconds timeout)
{
	return m_server ? m_server->WaitForWrites(timeout) : true;
}

bool SessionServer::CloseRequested() const
{
	return m_executor && m_executor->CloseRequested();
}

std::uint16_t SessionServer::Port() const
{
	return m_server ? m_server->Port() : 0;
}

} // namespace neurus
