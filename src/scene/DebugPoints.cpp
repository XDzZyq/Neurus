/**
 * @file DebugPoints.cpp
 * @brief Implementation of DebugPoints point management.
 */

#include "DebugPoints.hpp"

namespace neurus
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DebugPoints::DebugPoints()
	: ObjectID(GOType::GO_DP, "DebugPoints")
{
}

DebugPoints::DebugPoints(RestoredId id)
	: ObjectID(id, GOType::GO_DP, "DebugPoints")
{
}

// ---------------------------------------------------------------------------
// Point management
// ---------------------------------------------------------------------------

void DebugPoints::PushDebugPoint(const glm::vec3& point)
{
	o_points.push_back(point);
}

void DebugPoints::PushDebugPoints(const std::vector<glm::vec3>& points)
{
	o_points.insert(
		o_points.end(),
		points.begin(),
		points.end()
	);
}

void DebugPoints::ClearPoints()
{
	o_points.clear();
}

} // namespace neurus
