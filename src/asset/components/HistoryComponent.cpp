/**
 * @file HistoryComponent.cpp
 * @brief Undo/redo stack (de)serialization for project persistence.
 */

#include "asset/components/HistoryComponent.h"

#include <memory>
#include <vector>

#include <cereal/types/memory.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/vector.hpp>

#include "core/Log.h"
#include "editor/operations/Operation.h"
#include "editor/operations/OperationManager.h"
#include "editor/operations/registrations/OperationRegistration.h"

namespace neurus::project
{

HistoryComponent::HistoryComponent(OperationManager& operations)
	: m_operations(&operations)
{}

void HistoryComponent::Save(cereal::JSONOutputArchive& ar) const
{
	// Both stacks hold polymorphic ops: cereal emits each op's concrete type
	// name plus payload, so loading reconstructs the right subclass.
	ar.setNextName("m_history");
	ar.startNode();
	ar(cereal::make_nvp("undo", m_operations->GetUndoStack()),
	   cereal::make_nvp("redo", m_operations->GetRedoStack()));
	ar.finishNode();
}

void HistoryComponent::Load(cereal::JSONInputArchive& ar)
{
	try
	{
		std::vector<std::unique_ptr<Operation>> undo, redo;
		ar.setNextName("m_history");
		ar.startNode();
		ar(cereal::make_nvp("undo", undo),
		   cereal::make_nvp("redo", redo));
		ar.finishNode();
		m_operations->RestoreHistory(std::move(undo), std::move(redo));
	}
	catch (const cereal::Exception& e)
	{
		// Missing/legacy "m_history" node: start with a clean history rather
		// than aborting the whole load. Expected for older files, so logged
		// at info level rather than as an error.
		NEURUS_LOG("HistoryComponent::Load: " << e.what() << " - starting with empty history.");
		m_operations->Clear();
	}
}

} // namespace neurus::project
