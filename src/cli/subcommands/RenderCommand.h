#pragma once

#include "cli/SubCommand.h"

#include <limits>
#include <string>

namespace neurus::cli
{

/// `neurus render`: inspect and change rendering settings.
class RenderCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	CliExit ExecuteGet();
	CliExit ExecuteSet();

	CLI::App* m_app = nullptr;
	CLI::App* m_getApp = nullptr;
	CLI::App* m_setApp = nullptr;

	std::string m_session;
	std::string m_format;
	int         m_timeoutSeconds = 10;

	std::string m_pipeline;
	std::string m_aa;
	std::string m_ao;
	std::string m_shadow;
	std::string m_ssr;
	double      m_gamma = std::numeric_limits<double>::quiet_NaN();
	double      m_shadowBias = std::numeric_limits<double>::quiet_NaN();
	double      m_aoRadius = std::numeric_limits<double>::quiet_NaN();
	double      m_fxaaSubpix = std::numeric_limits<double>::quiet_NaN();
	double      m_fxaaEdgeThreshold = std::numeric_limits<double>::quiet_NaN();
	double      m_fxaaEdgeThresholdMin = std::numeric_limits<double>::quiet_NaN();
	int         m_aoKsize = std::numeric_limits<int>::min();
	int         m_samplePf = std::numeric_limits<int>::min();
	int         m_samplingCount = std::numeric_limits<int>::min();
	int         m_samplingMode = std::numeric_limits<int>::min();
	std::string m_transparent;
	std::string m_debugDraw;
};

} // namespace neurus::cli
