#pragma once

#include "asset/data/RenderGraphData.hpp"
#include "editor/events/GraphEvents.hpp"
#include "asset/data/ShaderData.hpp"
#include "editor/operations/GraphOperation.hpp"

namespace neurus {

/** @brief Toggles membership of the default graph created by an explicit edit. */
class CreateRenderGraphOp : public GraphOperation
{
public:
	CreateRenderGraphOp() = default;

	explicit CreateRenderGraphOp(int uid)
		: m_uid(uid)
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.Emit(RenderGraphMembershipChanged{m_uid, m_present, m_detached});
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		auto inverse = std::make_unique<CreateRenderGraphOp>(m_uid);
		inverse->m_present = !m_present;
		inverse->m_detached = m_detached;
		return inverse;
	}

	std::string Label() const override { return "Create Render Graph"; }

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("present", m_present));
		if (m_present)
		{
			ar(cereal::make_nvp("uid", m_uid));
			return;
		}

		// A saved redo entry can restore the original UID even after restart.
		// Creation always starts with the default topology, so only its identity
		// needs to survive outside the pool. Loading UID reserves it for redo.
		if constexpr (Archive::is_loading::value)
			m_detached = std::make_shared<RenderGraphData>();
		ar(cereal::make_nvp("identity", static_cast<UID&>(*m_detached)));
		if constexpr (Archive::is_loading::value)
			m_uid = m_detached->GetObjectID();
	}

private:
	int m_uid = 0;
	bool m_present = true;
	std::shared_ptr<RenderGraphData> m_detached;
};

/// Pool membership is a separate edit from assigning the shader to a pass.
class CreatePassShaderOp : public GraphOperation
{
public:
	CreatePassShaderOp() = default;
	explicit CreatePassShaderOp(int uid) : m_uid(uid) {}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.Emit(PassShaderMembershipChanged{m_uid, m_present, m_detached});
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		auto inverse = std::make_unique<CreatePassShaderOp>(*this);
		inverse->m_present = !m_present;
		return inverse;
	}

	std::string Label() const override { return "Create Pass Shader"; }
	bool ChangesTopology() const override { return false; }

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), CEREAL_NVP(m_present));
		if (!m_present)
			ar(CEREAL_NVP(m_detached));
	}

private:
	int m_uid = 0;
	bool m_present = true;
	std::shared_ptr<ShaderData> m_detached;
};

} // namespace neurus
