/**
 * @file RenderFrameSnapshot.cpp
 * @brief Out-of-line half of RenderFrameSnapshot: the reuse path.
 *
 * Clear() lives here rather than in the header because it is the one operation
 * that is not a field read: it resets the frame in place so a producer can
 * refill one snapshot per frame instead of allocating a fresh one, keeping the
 * draw lists' capacity.
 *
 * Vulkan-free, and must stay so: the snapshot describes the frame, not the GPU.
 */

#include "RenderFrameSnapshot.h"

namespace neurus
{

void RenderFrameSnapshot::Clear()
{
	config = RenderConfig{};

	hasScene = false;
	hasCamera = false;
	camera = CameraSnapshot{};

	// clear(), not a fresh vector: the capacity is the point of reusing a snapshot.
	meshes.clear();
	lights.clear();

	hasEnvironment = false;
	environmentId = 0;
	activeObjectId = 0;

	hasDebugDraw = false;
	debugDraw.Clear();
	gizmoDraw.Clear();
}

} // namespace neurus
