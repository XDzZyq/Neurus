/**
 * @file HistoryComponent.h
 * @brief Serializes an OperationManager's undo/redo stacks into a project.
 *
 * Save writes both stacks as arrays of polymorphic Operation pointers
 * (cereal auto-emits each op's concrete type); Load reconstructs them and
 * hands them back through RestoreHistory().
 */

#pragma once

#include "asset/Serializable.h"

namespace neurus { class OperationManager; }

namespace neurus::project
{

/**
 * @brief Persists an OperationManager's undo/redo history in a project file.
 */
class HistoryComponent : public Serializable
{
public:
	explicit HistoryComponent(OperationManager& operations);

	const char* Key() const noexcept override { return "m_history"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;

private:
	OperationManager* m_operations;
};

} // namespace neurus::project
