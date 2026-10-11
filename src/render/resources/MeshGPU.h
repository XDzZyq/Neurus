#pragma once

#include "../buffers/IndexBuffer.h"
#include "../buffers/VertexBuffer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace neurus {

/**
 * @brief GPU-side mesh resources owned by the Renderer layer.
 *
 * Holds the device-local vertex and index buffers for a mesh, together with
 * the vertex and index counts. The default-constructed state is empty: both
 * buffers are null and the counts are zero.
 *
 * Non-copyable (GPU resource handles are move-only).
 */
struct MeshGPU
{
	std::unique_ptr<VertexBuffer> vertexBuffer;
	std::unique_ptr<IndexBuffer> indexBuffer;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
};

/**
 * @brief Per-mesh push-constant block sent to the vertex shader.
 *
 * Two mat4s plus a uint32 (144 bytes) to satisfy Vulkan's 16-byte alignment
 * rule for push constants. objectID is forwarded to the fragment shader and
 * rasterized into the ID-buffer attachment.
 */
struct alignas(16) MeshPushConstants
{
	glm::mat4 model;           ///< Local-to-world transform (offset 0)
	glm::mat4 normalMatrix;    ///< 3x3 in upper-left of mat4 (offset 64)
	uint32_t objectID;         ///< Per-object identifier for ID buffer (offset 128)
};
static_assert(sizeof(MeshPushConstants) == 144,
              "MeshPushConstants must be 144 bytes (128 model + 4 objectID + 12 pad)");

} // namespace neurus
