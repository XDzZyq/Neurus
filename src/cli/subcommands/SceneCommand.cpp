#include "cli/subcommands/SceneCommand.h"

#include "cli/CommandClient.h"
#include "cli/Output.h"
#include "command/Commands.h"

#include <iostream>
#include <string>

namespace neurus::cli
{
namespace
{

void PrintSceneList(const command::Json& scene, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"scene", scene}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	const auto& objects = scene["objects"];
	std::cout << "uid\tname\ttype\tposition\n";
	for (const auto& object : objects)
	{
		std::cout << object.value("uid", 0) << '\t'
		          << object.value("name", std::string{}) << '\t'
		          << object.value("type", std::string{}) << '\t';
		if (object.contains("position"))
		{
			const auto& position = object["position"];
			std::cout << position[0].get<float>() << ' ' << position[1].get<float>() << ' '
			          << position[2].get<float>();
		}
		std::cout << '\n';
	}
	std::cout << scene.value("count", 0) << " object(s)\n";
}

void PrintSceneInfo(const command::Json& info, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"scene", info}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	for (const char* key : {"objects", "meshes", "lights", "cameras", "environments",
	                        "sprites", "debug", "active_camera"})
	{
		if (info.contains(key))
			std::cout << key << ' ' << info[key].dump() << '\n';
	}
	std::cout << "selected";
	for (const auto& uid : info["selected"])
		std::cout << ' ' << uid.get<int>();
	std::cout << '\n';
	std::cout << "dirty " << (info.value("dirty", false) ? "yes" : "no") << '\n';
}

} // namespace

void SceneCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("scene", "Inspect the scene document.");
	m_app->require_subcommand(1);

	auto addCommon = [this](CLI::App& app)
	{
		AddSessionOption(app, m_session);
		AddFormatOption(app, m_format);
		app.add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
			->check(CLI::PositiveNumber);
	};

	m_listApp = m_app->add_subcommand("list", "List scene objects.");
	m_listApp->add_option("--type", m_type,
	                      "Object type: all, mesh, light, camera, environment, sprite, debug.");
	addCommon(*m_listApp);

	m_infoApp = m_app->add_subcommand("info", "Report scene counts, selection and camera.");
	addCommon(*m_infoApp);
}

bool SceneCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit SceneCommand::Execute()
{
	const OutputFormat format = Format(m_format);
	CliExit exitCode = CliExit::Ok;
	if (m_infoApp != nullptr && m_infoApp->parsed())
	{
		const auto result = RunSessionCommand(SessionName(m_session),
		                                      {command::kSceneInfo, {}}, format, exitCode,
		                                      m_timeoutSeconds);
		if (!result)
			return exitCode;
		PrintSceneInfo(result->details, format);
		return CliExit::Ok;
	}

	if (m_listApp == nullptr || !m_listApp->parsed())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "A scene action is required.");
		return CliExit::Usage;
	}

	command::CommandRequest request{command::kSceneList, {}};
	if (!m_type.empty())
		request.args["type"] = m_type;

	const auto result = RunSessionCommand(SessionName(m_session), request, format, exitCode,
	                                      m_timeoutSeconds);
	if (!result)
		return exitCode;

	PrintSceneList(result->details, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
