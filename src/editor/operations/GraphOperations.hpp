#pragma once

#include "asset/data/RenderGraphData.hpp"
#include "editor/events/GraphEvents.hpp"
#include "editor/operations/Operation.hpp"

namespace neurus {

/** @brief Toggles membership of the default graph created by an explicit edit. */
class CreateRenderGraphOp : public Operation
{
public:
	CreateRenderGraphOp() = default;

	explicit CreateRenderGraphOp(int uid)
		: m_uid(uid)
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.Emit(RenderGraphCreationRestored{m_uid, m_present, m_detached});
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

} // namespace neurus
