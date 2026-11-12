#include "cli/subcommands/RenderCommand.h"

#include "cli/CommandClient.h"
#include "cli/Output.h"
#include "command/Commands.h"

#include <cmath>
#include <iostream>
#include <string>

namespace neurus::cli
{
namespace
{

void PrintConfig(const command::Json& config, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"render", config}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	const char* fields[] = {
		"pipeline", "aa", "ao", "shadow", "ssr", "gamma", "shadow_bias",
		"ao_ksize", "ao_radius", "sample_pf", "sampling_count", "sampling_mode",
		"transparent", "debug_draw", "fxaa_subpix", "fxaa_edge_threshold",
		"fxaa_edge_threshold_min",
	};
	for (const char* field : fields)
	{
		if (!config.contains(field))
			continue;
		const command::Json& value = config[field];
		std::cout << field << ' ';
		if (value.is_string())
			std::cout << value.get<std::string>();
		else if (value.is_boolean())
			std::cout << (value.get<bool>() ? "yes" : "no");
		else
			std::cout << value.dump();
		std::cout << '\n';
	}
}

bool ParseBool(const std::string& text, bool& value)
{
	if (text == "true")
	{
		value = true;
		return true;
	}
	if (text == "false")
	{
		value = false;
		return true;
	}
	return false;
}

} // namespace

void RenderCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("render", "Inspect and change rendering settings.");
	m_app->require_subcommand(1);

	m_getApp = m_app->add_subcommand("get", "Report the render configuration.");
	AddSessionOption(*m_getApp, m_session);
	AddFormatOption(*m_getApp, m_format);
	m_getApp->add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);

	m_setApp = m_app->add_subcommand("set", "Change render configuration values.");
	AddSessionOption(*m_setApp, m_session);
	AddFormatOption(*m_setApp, m_format);
	m_setApp->add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
	m_setApp->add_option("--pipeline", m_pipeline, "forward, deferred or custom0.")
		->check(CLI::IsMember({"forward", "deferred", "custom0"}));
	m_setApp->add_option("--aa", m_aa, "none, msaa or fxaa.")
		->check(CLI::IsMember({"none", "msaa", "fxaa"}));
	m_setApp->add_option("--ao", m_ao, "none or ssao.")
		->check(CLI::IsMember({"none", "ssao"}));
	m_setApp->add_option("--shadow", m_shadow, "none, shadow_mapping, sdf_soft or vssm.")
		->check(CLI::IsMember({"none", "shadow_mapping", "sdf_soft", "vssm"}));
	m_setApp->add_option("--ssr", m_ssr, "none, ray_marching, sdf_ray_marching or sdf_resolved.")
		->check(CLI::IsMember({"none", "ray_marching", "sdf_ray_marching", "sdf_resolved"}));
	m_setApp->add_option("--gamma", m_gamma, "Gamma correction factor.");
	m_setApp->add_option("--shadow-bias", m_shadowBias, "Shadow depth bias.");
	m_setApp->add_option("--ao-radius", m_aoRadius, "AO sample radius.");
	m_setApp->add_option("--ao-ksize", m_aoKsize, "AO kernel size.");
	m_setApp->add_option("--sample-pf", m_samplePf, "Samples per frame.");
	m_setApp->add_option("--sampling-count", m_samplingCount, "Accumulated frame count.");
	m_setApp->add_option("--sampling-mode", m_samplingMode, "0 = FixedEMA, 1 = MovingAvg.");
	m_setApp->add_option("--transparent", m_transparent, "Transparent background: true|false.")
		->check(CLI::IsMember({"true", "false"}));
	m_setApp->add_option("--debug-draw", m_debugDraw, "Debug overlay: true|false.")
		->check(CLI::IsMember({"true", "false"}));
	m_setApp->add_option("--fxaa-subpix", m_fxaaSubpix, "FXAA sub-pixel amount.");
	m_setApp->add_option("--fxaa-edge-threshold", m_fxaaEdgeThreshold, "FXAA edge threshold.");
	m_setApp->add_option("--fxaa-edge-threshold-min", m_fxaaEdgeThresholdMin,
	                     "FXAA minimum edge threshold.");
}

bool RenderCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit RenderCommand::Execute()
{
	if (m_getApp != nullptr && m_getApp->parsed())
		return ExecuteGet();
	if (m_setApp != nullptr && m_setApp->parsed())
		return ExecuteSet();
	PrintError(OutputFormat::Human, command::CommandErrorCode::InvalidArgument,
	           "A render action is required.");
	return CliExit::Usage;
}

CliExit RenderCommand::ExecuteGet()
{
	const OutputFormat format = Format(m_format);
	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_session),
	                                      {command::kRenderGet, {}}, format, exitCode,
	                                      m_timeoutSeconds);
	if (!result)
		return exitCode;
	PrintConfig(result->details["config"], format);
	return CliExit::Ok;
}

CliExit RenderCommand::ExecuteSet()
{
	command::Json args;
	auto setDouble = [&](const char* key, double value)
	{
		if (!std::isnan(value))
			args[key] = value;
	};
	auto setInt = [&](const char* key, int value)
	{
		if (value != std::numeric_limits<int>::min())
			args[key] = value;
	};
	auto setBool = [&](const char* key, const std::string& value)
	{
		bool parsed = false;
		if (!value.empty() && ParseBool(value, parsed))
			args[key] = parsed;
	};

	if (!m_pipeline.empty()) args["pipeline"] = m_pipeline;
	if (!m_aa.empty()) args["aa"] = m_aa;
	if (!m_ao.empty()) args["ao"] = m_ao;
	if (!m_shadow.empty()) args["shadow"] = m_shadow;
	if (!m_ssr.empty()) args["ssr"] = m_ssr;
	setDouble("gamma", m_gamma);
	setDouble("shadow_bias", m_shadowBias);
	setDouble("ao_radius", m_aoRadius);
	setDouble("fxaa_subpix", m_fxaaSubpix);
	setDouble("fxaa_edge_threshold", m_fxaaEdgeThreshold);
	setDouble("fxaa_edge_threshold_min", m_fxaaEdgeThresholdMin);
	setInt("ao_ksize", m_aoKsize);
	setInt("sample_pf", m_samplePf);
	setInt("sampling_count", m_samplingCount);
	setInt("sampling_mode", m_samplingMode);
	setBool("transparent", m_transparent);
	setBool("debug_draw", m_debugDraw);

	const OutputFormat format = Format(m_format);
	if (args.empty())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "No render setting to change was given.");
		return CliExit::Usage;
	}

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_session),
	                                      {command::kRenderSet, args}, format, exitCode,
	                                      m_timeoutSeconds);
	if (!result)
		return exitCode;
	PrintConfig(result->details["config"], format);
	return CliExit::Ok;
}

} // namespace neurus::cli
