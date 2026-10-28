/**
 * @file OperationManager.cpp
 * @brief Undo/redo stack management and synchronous operation replay.
 */

#include "editor/operations/OperationManager.h"

#include "core/Log.h"
#include "editor/operations/OperationContext.h"
#include "editor/events/EventBus.h"

namespace neurus {

const char* OperationManager::PhaseName(Phase phase)
{
	switch (phase)
	{
	case Phase::Idle: return "Idle";
	case Phase::Replaying: return "Replaying";
	case Phase::Script: return "Script";
	}
	return "Unknown";
}

OperationManager::OperationManager(EventQueue& bus, size_t maxUndoDepth)
	: m_bus(bus)
	, m_maxUndoDepth(maxUndoDepth)
{}

void OperationManager::Submit(std::unique_ptr<Operation> op)
{
	if (!op) return;

	// Replay re-runs the originating handler, which calls Submit again; suppress
	// it so history is not corrupted by its own playback. This is the EXPECTED
	// path on every undo/redo, so it is informational, not an error condition.
	if (m_phase == Phase::Replaying)
	{
		NEURUS_LOG("[OperationManager] Submit suppressed during replay (expected): "
		           << op->Label());
		return;
	}

	// A script run is one history entry: while the scope is open every op is
	// buffered instead of pushed, so the run cannot fragment into N steps. Before
	// the merge logic on purpose - coalescing is a per-entry concern and the
	// buffer is committed as a single composite by EndScriptScope().
	if (m_phase == Phase::Script)
	{
		m_scriptOps.push_back(std::move(op));
		return;
	}

	// Coalesce a stream of same-key edits into the undo-stack top; the live state
	// is already up to date, so merging only extends the recorded range.
	const std::string key = op->MergeKey();
	if (!key.empty() && !m_undo.empty() && m_undo.back()->MergeKey() == key)
	{
		NEURUS_LOG("[OperationManager] Submit: merged '" << op->Label()
		           << "' into the top entry (mergeKey='" << key << "')");
		m_undo.back()->MergeFrom(*op);
		m_redo.clear();
		++m_revision;
		return;
	}

	// A new forward edit normally invalidates the redo timeline; transparent ops
	// (e.g. selection) preserve it, which is safe because operations are
	// absolute state-sets.
	const bool preservesRedo = op->PreservesRedo();
	m_undo.push_back(std::move(op));
	if (!preservesRedo) m_redo.clear();
	EnforceUndoLimit();
	++m_revision;

	NEURUS_LOG("[OperationManager] Submit: '" << m_undo.back()->Label()
	           << "' (undo stack size " << m_undo.size()
	           << ", phase=" << PhaseName(m_phase)
	           << ", mergeKey='" << key << "')");
}

void OperationManager::Undo()
{
	if (m_undo.empty())
	{
		NEURUS_ERR("[OperationManager] Undo requested but the undo stack is empty");
		return;
	}

	NEURUS_LOG("[OperationManager] Undo: popping '" << m_undo.back()->Label()
	           << "' (undo size " << m_undo.size() << ", redo size " << m_redo.size() << ")");

	std::unique_ptr<Operation> g = std::move(m_undo.back());
	m_undo.pop_back();

	std::unique_ptr<Operation> inverse = g->Inverse();
	if (Replay(*inverse))
		m_redo.push_back(std::move(inverse));
	else
		NEURUS_ERR("[OperationManager] Undo of '" << g->Label()
		           << "' failed; nothing pushed to redo");
	++m_revision;

	NEURUS_LOG("[OperationManager] Undo done: (undo size " << m_undo.size()
	           << ", redo size " << m_redo.size() << ")");
}

void OperationManager::Redo()
{
	if (m_redo.empty())
	{
		NEURUS_ERR("[OperationManager] Redo requested but the redo stack is empty");
		return;
	}

	NEURUS_LOG("[OperationManager] Redo: popping '" << m_redo.back()->Label()
	           << "' (undo size " << m_undo.size() << ", redo size " << m_redo.size() << ")");

	std::unique_ptr<Operation> gInverse = std::move(m_redo.back());
	m_redo.pop_back();

	// The inverse of the inverse is the original forward edit.
	std::unique_ptr<Operation> forward = gInverse->Inverse();
	if (Replay(*forward))
		m_undo.push_back(std::move(forward));
	else
		NEURUS_ERR("[OperationManager] Redo of '" << gInverse->Label()
		           << "' failed; nothing pushed to undo");
	++m_revision;

	NEURUS_LOG("[OperationManager] Redo done: (undo size " << m_undo.size()
	           << ", redo size " << m_redo.size() << ")");
}

void OperationManager::Clear()
{
	NEURUS_LOG("[OperationManager] Clear: dropping " << m_undo.size()
	           << " undo + " << m_redo.size() << " redo entries and "
	           << m_scriptOps.size() << " buffered script op(s)");
	m_undo.clear();
	m_redo.clear();
	m_scriptOps.clear();
	m_scriptName.clear();
	++m_revision;
}

void OperationManager::BeginScriptScope(std::string name)
{
	// Nesting has no meaning: only the outermost run boundary could own the
	// commit, and a second Begin would silently discard the first run's buffer.
	if (m_phase != Phase::Idle)
	{
		NEURUS_ERR("[OperationManager] BeginScriptScope ignored: a scope is already open (phase="
		           << PhaseName(m_phase) << ")");
		return;
	}

	m_scriptOps.clear();
	// Only a successful Begin adopts the new label, so an ignored one cannot
	// rename the run whose buffer is still open.
	m_scriptName = std::move(name);
	m_phase = Phase::Script;
	NEURUS_LOG("[OperationManager] BeginScriptScope: buffering edits into one undo step");
}

void OperationManager::EndScriptScope()
{
	if (m_phase != Phase::Script)
	{
		NEURUS_ERR("[OperationManager] EndScriptScope ignored: no script scope is open (phase="
		           << PhaseName(m_phase) << ")");
		return;
	}

	// Restore Idle FIRST: the composite's Submit() below must take the normal
	// recording path, not re-enter the buffering branch.
	m_phase = Phase::Idle;

	std::vector<std::unique_ptr<Operation>> buffered = std::move(m_scriptOps);
	m_scriptOps.clear();

	// The label belongs to this run only: consume it on every exit path, the
	// empty run included, so it can never name the next run.
	std::string runName = std::move(m_scriptName);
	m_scriptName.clear();

	// A run that changed nothing (or only read) must not leave an empty entry on
	// the undo stack - it would make Ctrl+Z appear to do nothing.
	if (buffered.empty())
	{
		NEURUS_LOG("[OperationManager] EndScriptScope: the run recorded no edits");
		return;
	}

	// The run records as one labelled step: ScriptRunOp is a CompositeOp that
	// reports the run's name ("Run Script" when the host supplied none).
	NEURUS_LOG("[OperationManager] EndScriptScope: committing " << buffered.size()
	           << " buffered op(s) as one step");
	Submit(std::make_unique<ScriptRunOp>(std::move(buffered), std::move(runName)));
}

void OperationManager::RestoreHistory(std::vector<std::unique_ptr<Operation>> undo,
                                      std::vector<std::unique_ptr<Operation>> redo)
{
	NEURUS_LOG("[OperationManager] RestoreHistory: " << undo.size()
	           << " undo + " << redo.size() << " redo entries");
	m_undo = std::move(undo);
	m_redo = std::move(redo);
	EnforceUndoLimit();
	++m_revision;
}

HistoryView OperationManager::GetHistoryView() const
{
	HistoryView view;
	view.revision = m_revision;

	// Undo entries: oldest to newest (front to back); the last is the next Undo.
	view.undo.reserve(m_undo.size());
	for (const auto& op : m_undo)
		view.undo.push_back(op->Label());

	// Redo entries in replay order: the next Redo is the stack top, so walk it
	// top to bottom.
	view.redo.reserve(m_redo.size());
	for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it)
		view.redo.push_back((*it)->Label());

	return view;
}

bool OperationManager::Replay(Operation& op)
{
	NEURUS_LOG("[OperationManager] Replay: applying '" << op.Label()
	           << "' (phase -> Replaying)");

	OperationContext ctx{ m_bus };

	// m_phase must be restored on every exit path: a stuck Replaying phase would
	// silently suppress every later Submit(), making all further undo/redo
	// recording appear dead.
	const Phase prior = m_phase;
	m_phase = Phase::Replaying;

	// Contain handler exceptions: a single broken handler must not abort the
	// undo/redo call or propagate further. The op is reported as failed so the
	// caller drops it instead of moving it onto the opposite stack.
	try
	{
		op.Apply(ctx);
	}
	catch (const std::exception& ex)
	{
		NEURUS_ERR("[OperationManager] Replay of '" << op.Label()
		           << "' threw: " << ex.what());
		m_phase = prior;
		return false;
	}
	catch (...)
	{
		NEURUS_ERR("[OperationManager] Replay of '" << op.Label()
		           << "' threw a non-standard exception");
		m_phase = prior;
		return false;
	}
	m_phase = prior;
	return true;
}

void OperationManager::EnforceUndoLimit()
{
	if (m_maxUndoDepth == 0) return; // 0 = unbounded.
	if (m_undo.size() <= m_maxUndoDepth) return;

	// Evict the oldest entries (front) so the newest m_maxUndoDepth remain.
	const size_t excess = m_undo.size() - m_maxUndoDepth;
	NEURUS_LOG("[OperationManager] Evicting " << excess
	           << " oldest entries (undo cap " << m_maxUndoDepth << ")");
	m_undo.erase(m_undo.begin(), m_undo.begin() + excess);
}

} // namespace neurus
