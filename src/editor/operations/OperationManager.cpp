/**
 * @file OperationManager.cpp
 * @brief Undo/redo stack management and synchronous operation replay.
 */

#include "editor/operations/OperationManager.hpp"

#include "core/Log.hpp"
#include "editor/operations/GraphOperation.hpp"
#include "editor/operations/OperationContext.hpp"
#include "editor/operations/ScriptOperations.hpp"
#include "editor/events/EventBus.hpp"
#include <algorithm>

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
	: op_bus(bus)
	, op_maxUndoDepth(maxUndoDepth)
{}

void OperationManager::Submit(std::unique_ptr<Operation> op)
{
	if (!op) return;

	// Replay re-runs the originating handler, which calls Submit again; suppress
	// it so history is not corrupted by its own playback. This is the EXPECTED
	// path on every undo/redo, so it is informational, not an error condition.
	if (op_phase == Phase::Replaying)
	{
		NEURUS_LOG("[OperationManager] Submit suppressed during replay (expected): "
		           << op->Label());
		return;
	}

	// A script run is one history entry: while the scope is open every op is
	// buffered instead of pushed, so the run cannot fragment into N steps. Before
	// the merge logic on purpose - coalescing is a per-entry concern and the
	// buffer is committed as a single composite by EndScriptScope().
	if (op_phase == Phase::Script)
	{
		op_scriptOps.push_back(std::move(op));
		return;
	}

	// The controller already changed the document. Only its inverse is needed on rejection.
	try
	{
		GraphOperationBatch(*op).Commit(op_bus);
	}
	catch (...)
	{
		Apply(*op->Inverse(), false);
		GraphOperationBatch(*op).DiscardCreations(op_bus);
		throw;
	}
	if (!GraphOperationBatch::IsNoOp(*op))
		Record(std::move(op));
}

void OperationManager::Record(std::unique_ptr<Operation> op)
{
	// Coalesce a stream of same-key edits into the undo-stack top; the live state
	// is already up to date, so merging only extends the recorded range.
	const std::string key = op->MergeKey();
	if (!key.empty() && !op_undo.empty() && op_undo.back()->MergeKey() == key)
	{
		NEURUS_LOG("[OperationManager] Submit: merged '" << op->Label()
		           << "' into the top entry (mergeKey='" << key << "')");
		op_undo.back()->MergeFrom(*op);
		op_redo.clear();
		++op_revision;
		return;
	}

	// A new forward edit normally invalidates the redo timeline; transparent ops
	// (e.g. selection) preserve it, which is safe because operations are
	// absolute state-sets.
	const bool preservesRedo = op->PreservesRedo();
	op_undo.push_back(std::move(op));
	if (!preservesRedo) op_redo.clear();
	EnforceUndoLimit();
	++op_revision;

	NEURUS_LOG("[OperationManager] Submit: '" << op_undo.back()->Label()
	           << "' (undo stack size " << op_undo.size()
	           << ", phase=" << PhaseName(op_phase)
	           << ", mergeKey='" << key << "')");
}

void OperationManager::BeginCommandScope(std::string label)
{
	if (label.empty())
		label = "Command";
	BeginScriptScope(std::move(label));
}

std::string OperationManager::EndCommandScope(bool success)
{
	return EndScriptScope(success);
}

void OperationManager::Undo()
{
	if (op_undo.empty())
	{
		NEURUS_ERR("[OperationManager] Undo requested but the undo stack is empty");
		return;
	}

	NEURUS_LOG("[OperationManager] Undo: popping '" << op_undo.back()->Label()
	           << "' (undo size " << op_undo.size() << ", redo size " << op_redo.size() << ")");

	std::unique_ptr<Operation> g = std::move(op_undo.back());
	op_undo.pop_back();

	std::unique_ptr<Operation> inverse = g->Inverse();
	if (Replay(*inverse))
		op_redo.push_back(std::move(inverse));
	else
		NEURUS_ERR("[OperationManager] Undo of '" << g->Label()
		           << "' failed; nothing pushed to redo");
	++op_revision;

	NEURUS_LOG("[OperationManager] Undo done: (undo size " << op_undo.size()
	           << ", redo size " << op_redo.size() << ")");
}

void OperationManager::Redo()
{
	if (op_redo.empty())
	{
		NEURUS_ERR("[OperationManager] Redo requested but the redo stack is empty");
		return;
	}

	NEURUS_LOG("[OperationManager] Redo: popping '" << op_redo.back()->Label()
	           << "' (undo size " << op_undo.size() << ", redo size " << op_redo.size() << ")");

	std::unique_ptr<Operation> gInverse = std::move(op_redo.back());
	op_redo.pop_back();

	// The inverse of the inverse is the original forward edit.
	std::unique_ptr<Operation> forward = gInverse->Inverse();
	if (Replay(*forward))
		op_undo.push_back(std::move(forward));
	else
		NEURUS_ERR("[OperationManager] Redo of '" << gInverse->Label()
		           << "' failed; nothing pushed to undo");
	++op_revision;

	NEURUS_LOG("[OperationManager] Redo done: (undo size " << op_undo.size()
	           << ", redo size " << op_redo.size() << ")");
}

void OperationManager::Clear()
{
	NEURUS_LOG("[OperationManager] Clear: dropping " << op_undo.size()
	           << " undo + " << op_redo.size() << " redo entries and "
	           << op_scriptOps.size() << " buffered script op(s)");
	if (op_phase == Phase::Script)
	{
		RollbackGraphRequests();
		op_phase = Phase::Idle;
	}
	op_undo.clear();
	op_redo.clear();
	op_scriptOps.clear();
	op_scriptName.clear();
	++op_revision;
}

void OperationManager::BeginScriptScope(std::string name)
{
	// Nesting has no meaning: only the outermost run boundary could own the
	// commit, and a second Begin would silently discard the first run's buffer.
	if (op_phase != Phase::Idle)
	{
		NEURUS_ERR("[OperationManager] BeginScriptScope ignored: a scope is already open (phase="
		           << PhaseName(op_phase) << ")");
		return;
	}

	op_scriptOps.clear();
	op_scopeError.clear();
	// Only a successful Begin adopts the new label, so an ignored one cannot
	// rename the run whose buffer is still open.
	op_scriptName = std::move(name);
	op_phase = Phase::Script;
	NEURUS_LOG("[OperationManager] BeginScriptScope: buffering edits into one undo step");
}

void OperationManager::FlushGraphDrafts(std::vector<CpuGraphDiagnostic>* diagnostics)
{
	GraphOperationBatch(op_scriptOps).FlushDrafts(op_bus, diagnostics);
}

void OperationManager::RollbackGraphRequests()
{
	for (auto it = op_scriptOps.rbegin(); it != op_scriptOps.rend(); ++it)
	{
		if (GraphOperationBatch(**it).Empty())
			continue;
		Apply(*(*it)->Inverse(), false);
		GraphOperationBatch(**it).DiscardCreations(op_bus);
		it->reset();
	}
	std::erase(op_scriptOps, nullptr);
}

void OperationManager::RejectScope(const std::string& error)
{
	if (op_phase == Phase::Script && op_scopeError.empty())
		op_scopeError = error;
}

std::string OperationManager::EndScriptScope(bool success)
{
	if (op_phase != Phase::Script)
	{
		NEURUS_ERR("[OperationManager] EndScriptScope ignored: no script scope is open (phase="
		           << PhaseName(op_phase) << ")");
		return {};
	}

	std::string error = std::move(op_scopeError);
	if (success && error.empty())
	{
		try
		{
			GraphOperationBatch(op_scriptOps).Commit(op_bus);
			std::erase_if(op_scriptOps, [](const auto& op) { return GraphOperationBatch::IsNoOp(*op); });
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
		}
	}
	if (!success || !error.empty())
		RollbackGraphRequests();

	op_phase = Phase::Idle;

	std::vector<std::unique_ptr<Operation>> buffered = std::move(op_scriptOps);
	op_scriptOps.clear();

	// The label belongs to this run only: consume it on every exit path, the
	// empty run included, so it can never name the next run.
	std::string runName = std::move(op_scriptName);
	op_scriptName.clear();

	// A run that changed nothing (or only read) must not leave an empty entry on
	// the undo stack - it would make Ctrl+Z appear to do nothing.
	if (buffered.empty())
	{
		NEURUS_LOG("[OperationManager] EndScriptScope: the run recorded no edits");
		return error;
	}

	// The run records as one labelled step: ScriptRunOp is a CompositeOp that
	// reports the run's name ("Run Script" when the host supplied none).
	NEURUS_LOG("[OperationManager] EndScriptScope: committing " << buffered.size()
	           << " buffered op(s) as one step");
	Record(std::make_unique<ScriptRunOp>(std::move(buffered), std::move(runName)));
	return error;
}

void OperationManager::RestoreHistory(std::vector<std::unique_ptr<Operation>> undo,
                                      std::vector<std::unique_ptr<Operation>> redo)
{
	NEURUS_LOG("[OperationManager] RestoreHistory: " << undo.size()
	           << " undo + " << redo.size() << " redo entries");
	op_undo = std::move(undo);
	op_redo = std::move(redo);
	EnforceUndoLimit();
	++op_revision;
}

HistoryView OperationManager::GetHistoryView() const
{
	HistoryView view;
	view.revision = op_revision;

	// Undo entries: oldest to newest (front to back); the last is the next Undo.
	view.undo.reserve(op_undo.size());
	for (const auto& op : op_undo)
		view.undo.push_back(op->Label());

	// Redo entries in replay order: the next Redo is the stack top, so walk it
	// top to bottom.
	view.redo.reserve(op_redo.size());
	for (auto it = op_redo.rbegin(); it != op_redo.rend(); ++it)
		view.redo.push_back((*it)->Label());

	return view;
}

void OperationManager::Apply(Operation& op, bool historyReplay)
{
	// A synchronous restore handler may submit a record. Suppress it during
	// both history replay and compensation, and restore the enclosing phase.
	struct RestorePhase
	{
		Phase& current;
		Phase previous;
		~RestorePhase() { current = previous; }
	} guard{op_phase, op_phase};
	op_phase = Phase::Replaying;
	OperationContext ctx{op_bus, historyReplay};
	op.Apply(ctx);
	if (historyReplay)
	{
		try
		{
			GraphOperationBatch(op).Commit(op_bus);
		}
		catch (...)
		{
			Apply(*op.Inverse(), false);
			throw;
		}
	}
}

bool OperationManager::Replay(Operation& op)
{
	try
	{
		Apply(op, true);
		return true;
	}
	catch (const std::exception& failure)
	{
		NEURUS_ERR("[OperationManager] Replay of '" << op.Label() << "' failed: " << failure.what());
	}
	catch (...)
	{
		NEURUS_ERR("[OperationManager] Replay of '" << op.Label() << "' threw a non-standard exception");
	}
	return false;
}

void OperationManager::EnforceUndoLimit()
{
	if (op_maxUndoDepth == 0) return; // 0 = unbounded.
	if (op_undo.size() <= op_maxUndoDepth) return;

	// Evict the oldest entries (front) so the newest op_maxUndoDepth remain.
	const size_t excess = op_undo.size() - op_maxUndoDepth;
	NEURUS_LOG("[OperationManager] Evicting " << excess
	           << " oldest entries (undo cap " << op_maxUndoDepth << ")");
	op_undo.erase(op_undo.begin(), op_undo.begin() + excess);
}

} // namespace neurus
