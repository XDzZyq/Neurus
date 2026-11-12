#include "cli/subcommands/ObjectCommand.h"

#include "cli/CommandClient.h"
#include "cli/Output.h"
#include "command/Commands.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace neurus::cli
{
namespace
{

void AddCommonOptions(CLI::App& app, std::string& session, std::string& format,
                      int& timeoutSeconds)
{
	AddSessionOption(app, session);
	AddFormatOption(app, format);
	app.add_option("--timeout", timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
}

std::string Trim(const std::string& text)
{
	const auto first = text.find_first_not_of(" \t");
	if (first == std::string::npos)
		return {};
	const auto last = text.find_last_not_of(" \t");
	return text.substr(first, last - first + 1);
}

bool ParseFloat(const std::string& text, float& value, std::string& error)
{
	const std::string trimmed = Trim(text);
	if (trimmed.empty())
	{
		error = "Expected a number.";
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const float parsed = std::strtof(trimmed.c_str(), &end);
	if (errno != 0 || end == trimmed.c_str() || *end != '\0' || !std::isfinite(parsed))
	{
		error = "Not a finite number: " + trimmed;
		return false;
	}
	value = parsed;
	return true;
}

bool ParseVec3(const std::string& text, command::Json& value, std::string& error)
{
	std::vector<float> components;
	std::size_t start = 0;
	while (start <= text.size())
	{
		const std::size_t comma = text.find(',', start);
		const std::string piece = text.substr(
			start, comma == std::string::npos ? std::string::npos : comma - start);
		float component = 0.0f;
		if (!ParseFloat(piece, component, error))
			return false;
		components.push_back(component);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	if (components.size() != 3)
	{
		error = "Expected three comma-separated numbers.";
		return false;
	}
	value = command::Json::array({components[0], components[1], components[2]});
	return true;
}

bool ParseBool(const std::string& text, bool& value)
{
	if (text == "true" || text == "1" || text == "yes")
	{
		value = true;
		return true;
	}
	if (text == "false" || text == "0" || text == "no")
	{
		value = false;
		return true;
	}
	return false;
}

std::string FormatNumber(float value)
{
	std::string text = std::to_string(value);
	while (text.size() > 1 && text.back() == '0')
		text.pop_back();
	if (!text.empty() && text.back() == '.')
		text.pop_back();
	return text;
}

std::string FormatValue(const command::Json& value)
{
	if (value.is_array())
	{
		std::string text;
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			if (i != 0)
				text += ' ';
			text += FormatNumber(value[i].get<float>());
		}
		return text;
	}
	if (value.is_boolean())
		return value.get<bool>() ? "yes" : "no";
	if (value.is_number())
		return FormatNumber(value.get<float>());
	if (value.is_string())
		return value.get<std::string>();
	return value.dump();
}

void PrintFields(const command::Json& object, const std::vector<std::string>& fields)
{
	for (const auto& field : fields)
	{
		if (!object.contains(field))
			continue;
		std::cout << field << ' ' << FormatValue(object[field]) << '\n';
	}
}

void PrintObjectFull(const command::Json& object, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"object", object}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	std::cout << "object " << object.value("name", std::string{})
	          << " uid " << object.value("uid", 0)
	          << " type " << object.value("type", std::string{}) << '\n';
	PrintFields(object, {"position", "rotation", "scale", "viewport_visible",
	                     "render_visible", "active", "light_type", "power", "color",
	                     "radius", "cutoff", "outer_cutoff", "shadow", "material",
	                     "mesh_path", "fov", "target", "intensity",
	                     "environment_rotation"});
}

void PrintObjectSubset(const command::Json& object, const std::vector<std::string>& fields,
                       OutputFormat format)
{
	command::Json subset{{"name", object.value("name", std::string{})},
	                     {"uid", object.value("uid", 0)}};
	for (const auto& field : fields)
	{
		if (object.contains(field))
			subset[field] = object[field];
	}

	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"object", subset}};
		std::cout << json.dump(2) << '\n';
		return;
	}
	PrintFields(subset, fields);
}

void PrintDeleted(const command::Json& result, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"deleted", result["deleted"]},
		                         {"count", result.value("count", 0)}};
		std::cout << json.dump(2) << '\n';
		return;
	}
	for (const auto& object : result["deleted"])
	{
		std::cout << "deleted " << object.value("name", std::string{})
		          << " uid " << object.value("uid", 0) << '\n';
	}
}

bool BuildTarget(const std::string& name, int uid, const std::string& type,
                 command::Json& args, std::string& error)
{
	if (!name.empty() && uid > 0)
	{
		error = "Pass either --name or --uid, not both.";
		return false;
	}
	if (name.empty() && uid <= 0)
	{
		error = "Pass --name or --uid to identify the object.";
		return false;
	}
	if (!name.empty())
		args["name"] = name;
	if (uid > 0)
		args["uid"] = uid;
	if (!type.empty())
		args["type"] = type;
	return true;
}

} // namespace

void ObjectCommand::AddGetOptions(CLI::App& app)
{
	app.add_option("--name", m_getName, "Object name.");
	app.add_option("--uid", m_getUid, "Object UID.");
	app.add_option("--type", m_getType, "Object type filter.");
	app.add_flag("--position", m_getPosition, "Print only the position.");
	app.add_flag("--rotation", m_getRotation, "Print only the rotation.");
	app.add_flag("--scale", m_getScale, "Print only the scale.");
	app.add_flag("--visible", m_getVisible, "Print only visibility.");
}

void ObjectCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("object", "Read and write scene objects.");

	// `object ...` is the shorthand read; `object get ...` takes the same flags.
	AddGetOptions(*m_app);
	AddCommonOptions(*m_app, m_getSession, m_getFormat, m_getTimeout);

	CLI::App* getApp = m_app->add_subcommand("get", "Report one scene object.");
	AddGetOptions(*getApp);
	AddCommonOptions(*getApp, m_getSession, m_getFormat, m_getTimeout);

	m_loadApp = m_app->add_subcommand("load", "Load a mesh file into the scene.");
	m_loadApp->add_option("-f,--file", m_loadPath, "Mesh file to load.")->required();
	m_loadApp->add_option("--name", m_loadName, "Display name for the new object.");
	AddCommonOptions(*m_loadApp, m_loadSession, m_loadFormat, m_loadTimeout);

	m_addApp = m_app->add_subcommand("add", "Create an object in the scene.");
	m_addApp->add_option("--type", m_addType, "Object type. Currently only camera is supported.");
	m_addApp->add_option("--name", m_addName, "Display name for the new object.");
	m_addApp->add_option("--position", m_addPosition, "Position: x,y,z.");
	m_addApp->add_option("--target", m_addTarget, "Camera look-at target: x,y,z.");
	m_addApp->add_option("--fov", m_addFov, "Camera field of view in degrees.");
	m_addApp->add_option("--active", m_addActive, "Activate the new camera: true|false.");
	AddCommonOptions(*m_addApp, m_addSession, m_addFormat, m_addTimeout);

	m_duplicateApp = m_app->add_subcommand("duplicate", "Duplicate a mesh or camera.");
	m_duplicateApp->add_option("--name", m_duplicateName, "Source object name.");
	m_duplicateApp->add_option("--uid", m_duplicateUid, "Source object UID.");
	m_duplicateApp->add_option("--type", m_duplicateType, "Source object type filter.");
	m_duplicateApp->add_option("--rename", m_duplicateRename, "Name for the duplicate.");
	AddCommonOptions(*m_duplicateApp, m_duplicateSession, m_duplicateFormat,
	                 m_duplicateTimeout);

	m_setApp = m_app->add_subcommand("set", "Change properties of one scene object.");
	m_setApp->add_option("--name", m_setName, "Object name.");
	m_setApp->add_option("--uid", m_setUid, "Object UID.");
	m_setApp->add_option("--type", m_setType, "Object type filter.");
	m_setApp->add_option("--rename", m_setRename, "New display name.");
	m_setApp->add_option("--position", m_setPosition, "Position: x,y,z.");
	m_setApp->add_option("--rotation", m_setRotation, "Rotation in degrees: x,y,z.");
	m_setApp->add_option("--scale", m_setScale, "Scale: x,y,z.");
	m_setApp->add_option("--visible", m_setVisible, "Viewport and render visibility: true|false.");
	m_setApp->add_option("--viewport-visible", m_setViewportVisible,
	                     "Viewport visibility: true|false.");
	m_setApp->add_option("--render-visible", m_setRenderVisible,
	                     "Render visibility: true|false.");
	m_setApp->add_option("--active", m_setActive, "Camera activation: true|false.");
	m_setApp->add_option("--fov", m_setFov, "Camera field of view in degrees.");
	m_setApp->add_option("--target", m_setTarget, "Camera look-at target: x,y,z.");
	m_setApp->add_option("--power", m_setPower, "Light power.");
	m_setApp->add_option("--color", m_setColor, "Light color: r,g,b.");
	m_setApp->add_option("--radius", m_setRadius, "Light radius.");
	m_setApp->add_option("--cutoff", m_setCutoff, "Spot inner cutoff cosine.");
	m_setApp->add_option("--outer-cutoff", m_setOuterCutoff, "Spot outer cutoff cosine.");
	m_setApp->add_option("--shadow", m_setShadow, "Shadow casting: true|false.");
	m_setApp->add_option("--material", m_setMaterial, "Material shading: true|false.");
	m_setApp->add_option("--intensity", m_setIntensity, "Environment intensity.");
	m_setApp->add_option("--environment-rotation", m_setEnvironmentRotation,
	                     "Environment rotation in degrees.");
	AddCommonOptions(*m_setApp, m_setSession, m_setFormat, m_setTimeout);

	m_deleteApp = m_app->add_subcommand("delete", "Remove scene objects.");
	m_deleteApp->add_option("--name", m_deleteName, "Object name.");
	m_deleteApp->add_option("--uid", m_deleteUid, "Object UID.");
	m_deleteApp->add_option("--type", m_deleteType, "Object type filter.");
	m_deleteApp->add_option("--uids", m_deleteUids, "Comma-separated object UIDs.");
	AddCommonOptions(*m_deleteApp, m_deleteSession, m_deleteFormat, m_deleteTimeout);
}

bool ObjectCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit ObjectCommand::Execute()
{
	if (m_loadApp != nullptr && m_loadApp->parsed())
		return ExecuteLoad();
	if (m_addApp != nullptr && m_addApp->parsed())
		return ExecuteAdd();
	if (m_setApp != nullptr && m_setApp->parsed())
		return ExecuteSet();
	if (m_duplicateApp != nullptr && m_duplicateApp->parsed())
		return ExecuteDuplicate();
	if (m_deleteApp != nullptr && m_deleteApp->parsed())
		return ExecuteDelete();
	// Both `object ...` and `object get ...` read the same member options.
	return ExecuteGet();
}

CliExit ObjectCommand::ExecuteGet()
{
	const OutputFormat format = Format(m_getFormat);
	command::Json args;
	std::string error;
	if (!BuildTarget(m_getName, m_getUid, m_getType, args, error))
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_getSession),
	                                      {command::kObjectGet, args}, format, exitCode,
	                                      m_getTimeout);
	if (!result)
		return exitCode;

	const command::Json& object = result->details["object"];
	std::vector<std::string> fields;
	if (m_getPosition)
		fields.push_back("position");
	if (m_getRotation)
		fields.push_back("rotation");
	if (m_getScale)
		fields.push_back("scale");
	if (m_getVisible)
	{
		fields.push_back("viewport_visible");
		fields.push_back("render_visible");
	}
	if (fields.empty())
		PrintObjectFull(object, format);
	else
		PrintObjectSubset(object, fields, format);
	return CliExit::Ok;
}

CliExit ObjectCommand::ExecuteLoad()
{
	const OutputFormat format = Format(m_loadFormat);
	if (m_loadPath.empty())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "--file is required.");
		return CliExit::Usage;
	}

	std::error_code pathError;
	const auto absolute = std::filesystem::absolute(m_loadPath, pathError);
	command::Json args{{"path", pathError ? m_loadPath : absolute.string()}};
	if (!m_loadName.empty())
		args["name"] = m_loadName;

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_loadSession),
	                                      {command::kObjectLoad, args}, format, exitCode,
	                                      m_loadTimeout);
	if (!result)
		return exitCode;

	PrintObjectFull(result->details["object"], format);
	return CliExit::Ok;
}

CliExit ObjectCommand::ExecuteAdd()
{
	const OutputFormat format = Format(m_addFormat);
	if (m_addType.empty())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "--type is required.");
		return CliExit::Usage;
	}

	command::Json args{{"type", m_addType}};
	std::string error;
	if (!m_addName.empty())
		args["name"] = m_addName;
	if (!m_addPosition.empty())
	{
		command::Json value;
		if (!ParseVec3(m_addPosition, value, error))
		{
			PrintError(format, command::CommandErrorCode::InvalidArgument,
			           "--position: " + error);
			return CliExit::Usage;
		}
		args["position"] = std::move(value);
	}
	if (!m_addTarget.empty())
	{
		command::Json value;
		if (!ParseVec3(m_addTarget, value, error))
		{
			PrintError(format, command::CommandErrorCode::InvalidArgument,
			           "--target: " + error);
			return CliExit::Usage;
		}
		args["target"] = std::move(value);
	}
	if (!m_addFov.empty())
	{
		float value = 0.0f;
		if (!ParseFloat(m_addFov, value, error))
		{
			PrintError(format, command::CommandErrorCode::InvalidArgument,
			           "--fov: " + error);
			return CliExit::Usage;
		}
		args["fov"] = value;
	}
	if (!m_addActive.empty())
	{
		bool value = false;
		if (!ParseBool(m_addActive, value))
		{
			PrintError(format, command::CommandErrorCode::InvalidArgument,
			           "--active: expected true or false.");
			return CliExit::Usage;
		}
		args["active"] = value;
	}

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_addSession),
	                                      {command::kObjectAdd, args}, format, exitCode,
	                                      m_addTimeout);
	if (!result)
		return exitCode;

	PrintObjectFull(result->details["object"], format);
	return CliExit::Ok;
}

CliExit ObjectCommand::ExecuteDuplicate()
{
	const OutputFormat format = Format(m_duplicateFormat);
	command::Json args;
	std::string error;
	if (!BuildTarget(m_duplicateName, m_duplicateUid, m_duplicateType, args, error))
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}
	if (!m_duplicateRename.empty())
		args["rename"] = m_duplicateRename;

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_duplicateSession),
	                                      {command::kObjectDuplicate, args}, format, exitCode,
	                                      m_duplicateTimeout);
	if (!result)
		return exitCode;

	PrintObjectFull(result->details["object"], format);
	return CliExit::Ok;
}

CliExit ObjectCommand::ExecuteSet()
{
	const OutputFormat format = Format(m_setFormat);
	command::Json args;
	std::string error;
	if (!BuildTarget(m_setName, m_setUid, m_setType, args, error))
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	auto setVec3 = [&](const char* key, const std::string& text) -> bool
	{
		if (text.empty())
			return true;
		command::Json value;
		if (!ParseVec3(text, value, error))
		{
			error = std::string("--") + key + ": " + error;
			return false;
		}
		args[key] = std::move(value);
		return true;
	};
	auto setNumber = [&](const char* key, const std::string& text) -> bool
	{
		if (text.empty())
			return true;
		float value = 0.0f;
		if (!ParseFloat(text, value, error))
		{
			error = std::string("--") + key + ": " + error;
			return false;
		}
		args[key] = value;
		return true;
	};
	auto setBool = [&](const char* key, const std::string& text) -> bool
	{
		if (text.empty())
			return true;
		bool value = false;
		if (!ParseBool(text, value))
		{
			error = std::string("--") + key + ": expected true or false.";
			return false;
		}
		args[key] = value;
		return true;
	};

	const bool parsed = setVec3("position", m_setPosition)
		&& setVec3("rotation", m_setRotation)
		&& setVec3("scale", m_setScale)
		&& setVec3("target", m_setTarget)
		&& setVec3("color", m_setColor)
		&& setNumber("fov", m_setFov)
		&& setNumber("power", m_setPower)
		&& setNumber("radius", m_setRadius)
		&& setNumber("cutoff", m_setCutoff)
		&& setNumber("outer_cutoff", m_setOuterCutoff)
		&& setNumber("intensity", m_setIntensity)
		&& setNumber("environment_rotation", m_setEnvironmentRotation)
		&& setBool("visible", m_setVisible)
		&& setBool("viewport_visible", m_setViewportVisible)
		&& setBool("render_visible", m_setRenderVisible)
		&& setBool("active", m_setActive)
		&& setBool("shadow", m_setShadow)
		&& setBool("material", m_setMaterial);
	if (!parsed)
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}
	if (!m_setRename.empty())
		args["rename"] = m_setRename;

	// name/uid/type are target keys; anything beyond them is a property write.
	if (args.size() <= (m_setName.empty() ? 0 : 1) + (m_setUid > 0 ? 1 : 0)
		+ (m_setType.empty() ? 0 : 1))
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "No property to set was given.");
		return CliExit::Usage;
	}

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_setSession),
	                                      {command::kObjectSet, args}, format, exitCode,
	                                      m_setTimeout);
	if (!result)
		return exitCode;

	PrintObjectFull(result->details["object"], format);
	return CliExit::Ok;
}

CliExit ObjectCommand::ExecuteDelete()
{
	const OutputFormat format = Format(m_deleteFormat);
	command::Json args;
	std::string error;

	if (!m_deleteUids.empty())
	{
		if (!m_deleteName.empty() || m_deleteUid > 0)
		{
			PrintError(format, command::CommandErrorCode::InvalidArgument,
			           "Pass either --uids or --name/--uid, not both.");
			return CliExit::Usage;
		}
		command::Json uids = command::Json::array();
		std::size_t start = 0;
		while (start <= m_deleteUids.size())
		{
			const std::size_t comma = m_deleteUids.find(',', start);
			const std::string piece = Trim(m_deleteUids.substr(
				start, comma == std::string::npos ? std::string::npos : comma - start));
			char* end = nullptr;
			const long uid = std::strtol(piece.c_str(), &end, 10);
			if (piece.empty() || end == piece.c_str() || *end != '\0' || uid <= 0)
			{
				PrintError(format, command::CommandErrorCode::InvalidArgument,
				           "Invalid UID list: " + m_deleteUids);
				return CliExit::Usage;
			}
			uids.push_back(static_cast<int>(uid));
			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}
		args["uids"] = std::move(uids);
	}
	else if (!BuildTarget(m_deleteName, m_deleteUid, m_deleteType, args, error))
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_deleteSession),
	                                      {command::kObjectDelete, args}, format, exitCode,
	                                      m_deleteTimeout);
	if (!result)
		return exitCode;

	PrintDeleted(result->details, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
