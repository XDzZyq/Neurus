/**
 * @file ScriptOperations.hpp
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

#include "editor/operations/Operation.hpp"

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
class ScriptRunOp : public NamedCompositeOp
{
public:
	ScriptRunOp() = default;
	explicit ScriptRunOp(std::vector<std::unique_ptr<Operation>> ops, std::string name = {})
		: NamedCompositeOp(std::move(ops), std::move(name)) {}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<ScriptRunOp>(InvertedSequence(), m_name);
	}
	std::string Label() const override { return m_name.empty() ? "Run Script" : m_name; }
};

} // namespace neurus
