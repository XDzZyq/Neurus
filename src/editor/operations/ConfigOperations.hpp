/**
 * @file ConfigOperations.hpp
 * @brief Undoable operation for whole-RenderConfig edits.
 *
 * RenderConfig is scene-level (not per-object) state, so it does not fit the
 * UID-based TransitionOp: SetRenderConfigOp stores the absolute before/after
 * values and replays by dispatching the same event the UI emits.
 *
 * Config edits are deliberately NOT mergeable: continuous slider drags are
 * bounded by the controller's edit gesture instead, collapsing one drag to a
 * single op recorded on release.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "editor/events/ConfigEvents.hpp"
#include "editor/operations/Operation.hpp"
#include "core/RenderConfig.hpp"

namespace neurus {

/**
 * @brief Absolute whole-RenderConfig edit (before -> after), non-mergeable.
 */
class SetRenderConfigOp : public Operation
{
public:
	SetRenderConfigOp() = default;

	SetRenderConfigOp(std::optional<RenderConfig> before, std::optional<RenderConfig> after)
		: m_before(before.value_or(RenderConfig{}))
		, m_after(after.value_or(RenderConfig{}))
		, m_beforePresent(before.has_value())
		, m_afterPresent(after.has_value())
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.Emit(RenderConfigChangedEvent{ m_after, !m_afterPresent });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetRenderConfigOp>(
			m_afterPresent ? std::optional<RenderConfig>(m_after) : std::nullopt,
			m_beforePresent ? std::optional<RenderConfig>(m_before) : std::nullopt);
	}

	std::string Label() const override { return "Render Config"; }

	/** @brief Serializes the before/after RenderConfig endpoints. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		if constexpr (Archive::is_loading::value)
		{
			// Older histories always stored both endpoints.
			m_beforePresent = m_afterPresent = true;
			archive::Optional(ar, cereal::make_nvp("before_present", m_beforePresent),
			                  cereal::make_nvp("after_present", m_afterPresent));
		}
		if (m_beforePresent)
			ar(cereal::make_nvp("before", m_before));
		if (m_afterPresent)
			ar(cereal::make_nvp("after", m_after));
		if constexpr (Archive::is_saving::value)
		{
			ar(cereal::make_nvp("before_present", m_beforePresent),
			   cereal::make_nvp("after_present", m_afterPresent));
		}
	}

private:
	RenderConfig m_before;
	RenderConfig m_after;
	bool m_beforePresent = true;
	bool m_afterPresent = true;
};

} // namespace neurus
