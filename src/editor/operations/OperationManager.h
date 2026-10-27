/**
 * @file OperationManager.h
 * @brief Undo/redo history over event-replay operations.
 *
 * Holds two stacks of forward operations: undo emits the top op's Inverse()
 * synchronously (re-running the controller handler), redo emits the inverse of
 * the inverse. Replay is guarded by Phase::Replaying, so the Submit() the
 * re-run handler performs is suppressed rather than corrupting the stacks.
 *
 * A third phase, Phase::Script, exists for script runs: opening the script scope
 * makes Submit() buffer into m_scriptOps instead of the undo stack, so the whole
 * run can be committed as one CompositeOp - one history entry - when the scope
 * closes. Without it a single run would fragment into one step per property
 * write, which is not what "run script" means to a user.
 *
 * Needs no scene or resource access: operations are pure value descriptors that
 * dispatch events carrying integer UIDs, resolved by the handlers at replay
 * time. Implements IOperationSink so controllers record without knowing the
 * concrete type.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "editor/operations/HistoryView.h"
#include "editor/operations/IOperationSink.h"
#include "editor/operations/Operation.h"

namespace neurus {

class EventQueue;

/**
 * @brief Owns the undo/redo history and drives synchronous replay.
 */
class OperationManager : public IOperationSink
{
public:
	/** @brief Default cap on undo-stack depth (per Issue #19). */
	static constexpr size_t kDefaultMaxUndoDepth = 256;

	/**
	 * @brief Constructs the manager.
	 * @param bus Event queue operations replay their events on.
	 * @param maxUndoDepth Cap on undo-stack entries; oldest are evicted past it.
	 */
	OperationManager(EventQueue& bus, size_t maxUndoDepth = kDefaultMaxUndoDepth);

	// --- IOperationSink ---
	void Submit(std::unique_ptr<Operation> op) override;
	void BeginScriptScope() override;
	void EndScriptScope() override;

	// --- History control ---
	/** @brief Undoes the most recent operation (no-op if none). */
	void Undo();
	/** @brief Redoes the most recently undone operation (no-op if none). */
	void Redo();

	bool CanUndo() const { return !m_undo.empty(); }
	bool CanRedo() const { return !m_redo.empty(); }

	/**
	 * @brief Builds a read-only snapshot of both stacks for display.
	 * @return Labels (oldest-first undo, replay-order redo) plus the revision.
	 */
	HistoryView GetHistoryView() const;

	/** @brief Clears both stacks and any half-open script buffer. */
	void Clear();

	// --- Persistence hooks ---
	/** @brief Undo stack, oldest to newest (next-undo last). Read-only. */
	const std::vector<std::unique_ptr<Operation>>& GetUndoStack() const { return m_undo; }
	/** @brief Redo stack, bottom to top (next-redo last). Read-only. */
	const std::vector<std::unique_ptr<Operation>>& GetRedoStack() const { return m_redo; }

	/**
	 * @brief Replaces both stacks wholesale (project load).
	 * @param undo New undo stack (ownership taken).
	 * @param redo New redo stack (ownership taken).
	 * @note Does NOT replay: the ops are absolute state-sets restored as-is; the
	 *       live scene is loaded separately.
	 */
	void RestoreHistory(std::vector<std::unique_ptr<Operation>> undo,
	                    std::vector<std::unique_ptr<Operation>> redo);

private:
	/**
	 * @brief Recording phase.
	 *
	 * Idle records normally; Replaying suppresses Submit() because undo/redo
	 * re-runs the originating handler; Script buffers Submit() into m_scriptOps
	 * so one script run lands as a single history entry.
	 */
	enum class Phase { Idle, Replaying, Script };

	/**
	 * @brief Emits an operation synchronously under the replay guard.
	 * @return true if the operation applied, false only if Apply() threw and the
	 *         op must be dropped.
	 */
	bool Replay(Operation& op);

	/**
	 * @brief Evicts the oldest undo entries so the stack stays within the depth.
	 */
	void EnforceUndoLimit();

	EventQueue& m_bus;

	std::vector<std::unique_ptr<Operation>> m_undo;
	std::vector<std::unique_ptr<Operation>> m_redo;

	/// Ops recorded while a script scope is open; committed as one CompositeOp.
	std::vector<std::unique_ptr<Operation>> m_scriptOps;

	/// Maximum undo-stack depth; oldest entries evicted past this.
	size_t m_maxUndoDepth = kDefaultMaxUndoDepth;

	/// Monotonic counter bumped on every stack change; drives UI change detection.
	uint64_t m_revision = 0;

	Phase m_phase = Phase::Idle;
};

} // namespace neurus
