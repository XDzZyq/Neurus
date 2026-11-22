#pragma once

#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "asset/data/shader/ShaderStage.hpp"
#include "asset/data/shader/ShaderUnit.hpp"
#include "core/UID.hpp"

#include <cereal/types/memory.hpp>
#include <cereal/types/polymorphic.hpp>

namespace neurus {

struct GraphStageEdit
{
	int uid;
	ShaderType stage;
	ShaderUnit before, after;

	template<class Archive>
	void serialize(Archive& ar) { ar(CEREAL_NVP(uid), CEREAL_NVP(stage), CEREAL_NVP(before), CEREAL_NVP(after)); }
};

/** Absolute before/after state for the parts of a graph changed by one edit. */
struct GraphEditState
{
	int graphUid = 0;
	std::optional<project::RenderGraphRecord> before, after;
	std::vector<GraphStageEdit> stages;
	std::vector<int> createdShaders;
	// Owned only while the created shader is detached on the redo stack.
	std::vector<std::shared_ptr<UID>> detached;

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(graphUid), CEREAL_NVP(before), CEREAL_NVP(after), CEREAL_NVP(stages),
		   CEREAL_NVP(createdShaders), CEREAL_NVP(detached));
	}
};

} // namespace neurus
