#include "editor/operations/GraphOperation.hpp"

#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "editor/events/GraphEvents.hpp"
#include "editor/operations/PassShaderStageOperation.hpp"

#include <map>

namespace neurus {

GraphOperationBatch::GraphOperationBatch(std::span<const std::unique_ptr<Operation>> requests)
{
	for (const auto& request : requests)
		Collect(*request);
}

void GraphOperationBatch::Collect(Operation& op)
{
	if (auto* edit = dynamic_cast<GraphOperation*>(&op))
		m_edits.push_back(edit);
	else if (auto* group = dynamic_cast<CompositeOp*>(&op))
		for (const auto& child : group->Children())
			Collect(*child);
}

bool GraphOperationBatch::IsNoOp(const Operation& op)
{
	const auto* stage = dynamic_cast<const SetPassShaderStageOp*>(&op);
	return stage && stage->IsNoOp();
}

void GraphOperationBatch::FlushDrafts(IEventQueue& events, std::vector<CpuGraphDiagnostic>* diagnostics) const
{
	// Several writes to the same stage can contain intermediate, incomplete GLSL.
	// Keep the first before-image and compile the final source once.
	std::map<std::pair<int, ShaderType>, std::vector<SetPassShaderStageOp*>> drafts;
	for (auto* edit : m_edits)
		if (auto* stage = dynamic_cast<SetPassShaderStageOp*>(edit); stage && stage->Draft() && !stage->IsNoOp())
			drafts[{stage->ShaderUid(), stage->Stage()}].push_back(stage);

	for (auto& [key, stages] : drafts)
	{
		auto& first = *stages.front();
		const ShaderUnit* changed = nullptr;
		bool portsChanged = false;
		const auto problems = diagnostics ? diagnostics->size() : 0;
		events.Emit(PassShaderDraftCompileRequested{key.first, key.second, *stages.back()->Draft(),
		                                           first.Before(), changed, portsChanged, diagnostics});
		if (diagnostics && diagnostics->size() != problems)
			continue;
		for (auto* edit : stages)
			if (edit != &first || !changed)
				edit->Discard();
		if (changed)
			first.Complete(*changed, portsChanged);
	}
}

void GraphOperationBatch::DiscardCreations(IEventQueue& events) const
{
	for (const auto* edit : m_edits)
		if (const auto uid = edit->CreatedResource())
			events.Emit(GraphCreationDiscarded{*uid});
}

void GraphOperationBatch::Commit(IEventQueue& events) const
{
	if (Empty())
		return;
	FlushDrafts(events);
	RenderGraphCommitRequested change;
	for (const auto* edit : m_edits)
	{
		if (IsNoOp(*edit))
			continue;
		change.topologyChanged |= edit->ChangesTopology();
		if (const auto* stage = dynamic_cast<const SetPassShaderStageOp*>(edit))
			change.shaderUids.insert(stage->ShaderUid());
	}
	if (change.topologyChanged || !change.shaderUids.empty())
		events.Emit(change);
}

} // namespace neurus
