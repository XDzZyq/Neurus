#pragma once

#include <string>

namespace neurus {

/// A synchronous request failed. An enclosing group retains its first error.
struct OperationRejected
{
	std::string error;
};

/** @brief Emitted when the user requests undo (Edit -> Undo / Ctrl+Z). */
struct UndoRequested {};

/** @brief Emitted when the user requests redo (Edit -> Redo / Ctrl+Shift+Z). */
struct RedoRequested {};

} // namespace neurus
