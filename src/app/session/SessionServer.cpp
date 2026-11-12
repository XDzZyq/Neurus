#include "app/session/SessionServer.h"

#include "app/session/SessionCommandExecutor.h"
#include "command/CommandDescriptor.h"
#include "command/CommandJson.h"
#include "ipc/IpcServer.h"
#include "ipc/RandomToken.h"
#include "ipc/SessionRegistry.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace neurus
{
namespace
{

std::string UtcTimestamp()
{
	// Calendar arithmetic over system_clock: no time_t and no gmtime, so the
	// same code serves every platform. snprintf keeps the output identical to
	// the previous strftime format without relying on std::format's chrono
	// support in the standard library at hand.
	const auto now = std::chrono::floor<std::chrono::seconds>(
		std::chrono::system_clock::now());
	const auto day = std::chrono::floor<std::chrono::days>(now);
	const std::chrono::year_month_day ymd{day};
	const std::chrono::hh_mm_ss hms{now - day};
	char buffer[32]{};
	std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
	              static_cast<int>(ymd.year()),
	              static_cast<int>(static_cast<unsigned>(ymd.month())),
	              static_cast<int>(static_cast<unsigned>(ymd.day())),
	              static_cast<int>(hms.hours().count()),
	              static_cast<int>(hms.minutes().count()),
	              static_cast<int>(hms.seconds().count()));
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
