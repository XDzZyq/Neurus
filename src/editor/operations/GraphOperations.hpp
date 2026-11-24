#pragma once

#include "editor/events/GraphEvents.hpp"
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
		ctx.events.Emit(RenderGraphMembershipChanged{m_uid, m_present});
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		auto inverse = std::make_unique<CreateRenderGraphOp>(m_uid);
		inverse->m_present = !m_present;
		return inverse;
	}

	std::optional<int> CreatedResource() const override { return m_uid; }

	std::string Label() const override { return "Create Render Graph"; }

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("present", m_present), cereal::make_nvp("uid", m_uid));
	}

private:
	int m_uid = 0;
	bool m_present = true;
};

} // namespace neurus
