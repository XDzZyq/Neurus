#pragma once

#include "editor/controllers/Controllers.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "asset/data/shader/ShaderUnit.hpp"
#include "editor/operations/GraphEditState.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace neurus {

/** Owns the Editor-thread graph edit scope, including publication and history. */
class RenderGraphController : public Controllers
{
public:
	void Init(ControllerContext& ctx) override;

private:
	void Edit(const ControllerContext& ctx, const std::function<void()>& action);
	RenderGraphData& Document(const ControllerContext& ctx);
	std::optional<CpuGraphDiagnostic> CompilePendingStages(const ControllerContext& ctx);
	void FlushShaders(const ControllerContext& ctx, std::vector<CpuGraphDiagnostic>* diagnostics = nullptr);
	void Validate(const ControllerContext& ctx) const;
	void Commit(const ControllerContext& ctx);
	void Rollback(const ControllerContext& ctx);
	void Clear();
	void Replay(const ControllerContext& ctx, GraphEditState& state, bool forward);
	void AssignShader(const ControllerContext& ctx, RenderGraphTopology::NodeType& node, std::shared_ptr<ShaderData> shader);
	bool EditShader(const ControllerContext& ctx, int uid, ShaderType stage, ShaderUnit candidate, bool fromStruct);
	void PublishShaderEdits(const ControllerContext& ctx, const GraphEditState& state, bool topologyChanged);

	bool m_script = false;
	bool m_created = false;
	std::optional<RenderGraphTopology> m_original;
	GraphEditState m_edit;
	std::map<std::pair<int, ShaderType>, bool> m_pendingStages;
	std::string m_error;
};

} // namespace neurus
