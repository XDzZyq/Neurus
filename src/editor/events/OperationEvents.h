#pragma once

namespace neurus {

/** @brief Emitted when the user requests undo (Edit -> Undo / Ctrl+Z). */
struct UndoRequested {};

/** @brief Emitted when the user requests redo (Edit -> Redo / Ctrl+Shift+Z). */
struct RedoRequested {};

} // namespace neurus
