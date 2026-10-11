#pragma once

#include "../Texture.h"

#include <memory>

namespace neurus {

/**
 * @brief GPU-side IBL environment resources owned by the Renderer layer.
 *
 * Holds the diffuse irradiance and specular prefiltered cubemap Textures
 * (Image + Sampler).
 *
 * Non-copyable (GPU resource handles are move-only).
 */
struct EnvironmentGPU
{
	std::unique_ptr<Texture> diffuseTexture;   ///< Diffuse irradiance cubemap (64 px, 1 mip)
	std::unique_ptr<Texture> specularTexture;  ///< Specular prefiltered cubemap (2048 px, 8 mips)
};

} // namespace neurus
