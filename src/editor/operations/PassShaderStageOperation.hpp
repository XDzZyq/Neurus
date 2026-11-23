#pragma once

#include "asset/data/shader/ShaderStage.hpp"
#include "asset/data/shader/ShaderUnit.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "editor/operations/GraphOperation.hpp"

#include <optional>

namespace neurus {

/// One pooled pass shader stage. Grouped text/IR requests compile at their first read or commit.
class SetPassShaderStageOp : public GraphOperation
{
public:
	SetPassShaderStageOp() = default;
	SetPassShaderStageOp(int uid, ShaderType stage, const ShaderUnit& before, ShaderUnit after, PassShaderEditSource source)
		: m_uid(uid), m_stage(stage), m_before(before), m_after(std::move(after)), m_draft(source)
	{}

	int ShaderUid() const { return m_uid; }
	ShaderType Stage() const { return m_stage; }
	const ShaderUnit& Before() const { return m_before; }
	std::optional<PassShaderEditSource> Draft() const { return m_draft; }
	bool ChangesTopology() const override { return m_portsChanged; }
	bool IsNoOp() const { return m_noOp; }

	void Complete(ShaderUnit after, bool portsChanged)
	{
		m_after = std::move(after);
		m_draft.reset();
		m_portsChanged = portsChanged;
	}

	void Discard() { m_noOp = true; }

	void Apply(OperationContext& ctx) override
	{
		if (!m_noOp)
			ctx.events.Emit(PassShaderStageChanged{m_uid, m_stage, m_after, !m_draft.has_value(), m_portsChanged, ctx.historyReplay});
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		auto inverse = std::make_unique<SetPassShaderStageOp>(*this);
		std::swap(inverse->m_before, inverse->m_after);
		return inverse;
	}

	std::string Label() const override { return "Edit Pass Shader Stage"; }

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), CEREAL_NVP(m_stage), CEREAL_NVP(m_before), CEREAL_NVP(m_after), CEREAL_NVP(m_portsChanged));
	}

private:
	int m_uid = 0;
	ShaderType m_stage{};
	ShaderUnit m_before, m_after;
	std::optional<PassShaderEditSource> m_draft;  ///< A draft may contain incomplete source.
	bool m_portsChanged = false;
	bool m_noOp = false;
};

} // namespace neurus
