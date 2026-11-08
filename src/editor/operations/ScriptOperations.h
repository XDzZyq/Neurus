/**
 * @file ScriptOperations.h
 * @brief Script-domain operations recorded on the shared undo history.
 *
 * A script run is a burst of ordinary edits that must collapse into one history
 * entry: ScriptRunOp is that entry, a CompositeOp carrying the run's label so the
 * History panel names the run instead of its first child.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#include "editor/operations/Operation.h"

namespace neurus {

/**
 * @brief One script run's edits, labelled with the name the run supplied.
 *
 * Same replay semantics as CompositeOp: Apply() runs the edits in forward order
 * and Inverse() returns the reversed, individually-inverted sequence. The label is
 * the run's name, defaulting to "Run Script" when it is empty; the name is
 * serialized with the edits, so the History panel shows the same text after a
 * project save and reload.
 */
class ScriptRunOp : public CompositeOp
{
public:
	ScriptRunOp() = default;
	explicit ScriptRunOp(std::vector<std::unique_ptr<Operation>> ops, std::string name = {});

	std::unique_ptr<Operation> Inverse() const override;
	std::string Label() const override { return m_name.empty() ? "Run Script" : m_name; }

	template<class Archive>
	void serialize(Archive& ar)
	{
		CompositeOp::serialize(ar);
		ar(CEREAL_NVP(m_name));
	}

private:
	std::string m_name; ///< Run-supplied label; empty means "Run Script".
};

/// @brief Constructs a script run from its forward-order edits and history label.
inline ScriptRunOp::ScriptRunOp(std::vector<std::unique_ptr<Operation>> ops, std::string name)
	: CompositeOp(std::move(ops))
	, m_name(std::move(name))
{}

/**
 * @brief Inverts as another ScriptRunOp carrying the same name, so the redo
 *        entry keeps the label.
 */
inline std::unique_ptr<Operation> ScriptRunOp::Inverse() const
{
	return std::make_unique<ScriptRunOp>(InvertedSequence(), m_name);
}

} // namespace neurus
