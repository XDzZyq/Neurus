#pragma once

#include "editor/operations/Operation.hpp"

#include <optional>
#include <span>

namespace neurus {

/// An edit whose CPU result must be validated before publication to the renderer.
class GraphOperation : public Operation
{
public:
	virtual std::optional<int> CreatedResource() const { return {}; }
	virtual bool ChangesTopology() const { return true; }
};

struct CPUGraphDiagnostic;

/// Short-lived view of the graph edits in one request, script run, or history entry.
class GraphOperationBatch
{
public:
	explicit GraphOperationBatch(Operation& request) { Collect(request); }
	explicit GraphOperationBatch(std::span<const std::unique_ptr<Operation>> requests);

	bool Empty() const { return m_edits.empty(); }
	void FlushDrafts(IEventQueue& events, std::vector<CPUGraphDiagnostic>* diagnostics = nullptr) const;
	void Commit(IEventQueue& events) const;
	void DiscardCreations(IEventQueue& events) const;
	static bool IsNoOp(const Operation& op);

private:
	void Collect(Operation& op);
	std::vector<GraphOperation*> m_edits;
};

} // namespace neurus
