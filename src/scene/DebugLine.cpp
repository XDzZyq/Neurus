/**
 * @file DebugLine.cpp
 * @brief Implementation of DebugLine vertex management.
 */

#include "DebugLine.hpp"

namespace neurus
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DebugLine::DebugLine()
	: ObjectID(GOType::GO_DL, "DebugLine")
{
}

DebugLine::DebugLine(RestoredId id)
	: ObjectID(id, GOType::GO_DL, "DebugLine")
{
}

// ---------------------------------------------------------------------------
// Vertex management
// ---------------------------------------------------------------------------

void DebugLine::PushDebugLine(const glm::vec3& start, const glm::vec3& end)
{
	o_vertices.push_back(start);
	o_vertices.push_back(end);
}

void DebugLine::PushDebugLines(const std::vector<glm::vec3>& vertices)
{
	o_vertices.insert(
		o_vertices.end(),
		vertices.begin(),
		vertices.end()
	);
}

void DebugLine::ClearVertices()
{
	o_vertices.clear();
}

} // namespace neurus
