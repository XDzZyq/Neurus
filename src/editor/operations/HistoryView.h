/**
 * @file HistoryView.h
 * @brief Read-only snapshot of the undo/redo stacks for display.
 *
 * A flat, Qt-free copy of the operation labels, produced on demand; the UI never
 * touches the live stacks or the operation objects.
 *
 * Ordering (both vectors read as a forward timeline top to bottom):
 * - `undo`  - already-applied operations, oldest first; the last entry is the
 *   next Undo target.
 * - `redo`  - undone operations available to reapply; the first entry is the
 *   next Redo target.
 *
 * `revision` monotonically increases whenever the stacks change, letting the
 * consumer skip rebuilding when nothing happened.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neurus {

/**
 * @brief Flat snapshot of the undo/redo history for read-only display.
 */
struct HistoryView
{
	std::vector<std::string> undo; ///< Applied ops, oldest to newest (next-undo last).
	std::vector<std::string> redo; ///< Undone ops, in replay order (next-redo first).
	uint64_t revision = 0;         ///< Bumped on every stack change.
};

} // namespace neurus
