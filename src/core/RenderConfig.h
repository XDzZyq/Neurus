/**
 * @file RenderConfig.h
 * @brief User-configurable rendering settings and algorithm selections.
 *
 * RenderConfig aggregates all rendering options that affect visual quality
 * and performance. These settings are read by the Renderer during pipeline
 * execution and can be toggled at runtime.
 *
 * Pure data class with query helpers - owned by the Editor (Editor::m_config).
 * Vulkan-free, so it lives in core/ and can be read by any layer; render
 * passes reach it by value-embedded pointer, read through Settings().
 *
 * @note This is NOT a per-frame struct - it persists across frames.
 */

#pragma once

#include <cstdint>

#include <cereal/cereal.hpp>

#include "core/Serialize.h"

namespace neurus
{

/**
 * @brief Rendering pipeline architecture selection.
 */
enum class RenderPipeLine : char
{
	Forward,  ///< Forward rendering (single-pass)
	Deferred, ///< Deferred rendering (multi-pass with G-Buffer) - current
	Custom0   ///< Custom pipeline slot (reserved)
};

/**
 * @brief Anti-aliasing algorithm.
 */
enum class AAAlg : char
{
	None, ///< No anti-aliasing
	MSAA, ///< Multi-sample anti-aliasing
	FXAA  ///< Fast approximate anti-aliasing
};

/**
 * @brief Ambient occlusion algorithm.
 */
enum class AOAlg : char
{
	None, ///< No ambient occlusion
	SSAO  ///< Screen-space ambient occlusion (current implementation)
};

/**
 * @brief Shadow rendering algorithm.
 */
enum class ShadowAlg : char
{
	None,          ///< No shadows
	ShadowMapping, ///< Standard shadow mapping (current implementation)
	SDFSoftShadow, ///< SDF-based soft shadows
	VSSM           ///< Variance soft shadow mapping
};

/**
 * @brief Screen-space reflection algorithm.
 */
enum class SSRAlg : char
{
	None,                    ///< No SSR
	RayMarching,             ///< Standard ray marching in screen space
	SDFRayMarching,          ///< SDF-accelerated ray marching
	SDFResolvedRayMarching   ///< SDF-resolved ray marching
};

/**
 * @brief Rendering configuration container for pipeline and algorithm settings.
 *
 * Provides the algorithm-selection enums and the quality parameters the
 * renderer queries to configure the pipeline dynamically.
 */
class RenderConfig
{
public:
	// --- Algorithm selection ---

	RenderPipeLine r_pipeline = RenderPipeLine::Deferred;  ///< Active rendering pipeline
	AAAlg         r_aa       = AAAlg::None;                ///< Anti-aliasing mode
	AOAlg         r_ao       = AOAlg::SSAO;                ///< Ambient occlusion algorithm
	ShadowAlg     r_shadow   = ShadowAlg::ShadowMapping;   ///< Shadow algorithm
	SSRAlg        r_ssr      = SSRAlg::None;               ///< Screen-space reflections

	// --- Quality parameters ---

	float   r_gamma       = 1.0f;     ///< Gamma correction factor
	float   r_shadow_bias = 0.02f;  ///< Depth bias for shadow acne prevention
	uint32_t r_sampling_mode = 0;  ///< Temporal sampling mode (0 = FixedEMA/8, 1 = MovingAvg)
	int32_t r_ao_ksize    = 16;       ///< AO kernel size (number of samples)
	float   r_ao_radius   = 0.5f;     ///< AO sample radius in world-space
	int32_t r_sample_pf   = 128;      ///< Samples per frame for progressive rendering
	int32_t r_sampling_count = 64;    ///< Scene frames accumulated after a restart before rendering stops (>= 1)
	bool    r_transparent = false;    ///< Transparent background (checkerboard instead of skybox)

	/**
	 * @brief Master switch for the viewport debug/gizmo overlay.
	 *
	 * Gates publication, not pipeline topology: when false a null debugDraw is
	 * published and the pass returns before touching any image, so toggling
	 * this costs no graph rebuild.
	 */
	bool    r_debug_draw  = true;

	// --- FXAA parameters ---
	float r_fxaa_subpix           = 0.75f;  ///< Sub-pixel aliasing removal amount (0.0-1.0)
	float r_fxaa_edge_threshold   = 0.166f;  ///< Edge detection threshold (0.063-0.333)
	float r_fxaa_edge_threshold_min = 0.0833f; ///< Minimum edge threshold (0.0312-0.0833)

public:
	RenderConfig() = default;

	/** @brief Value equality across all settings (used to skip no-op undo entries). */
	bool operator==(const RenderConfig&) const = default;

	// --- Cereal serialization ---

	/**
	 * @brief Serializes every setting, each one independently optional.
	 *
	 * No render setting is load-bearing for identity, so none is worth failing
	 * a load over: a key this build does not find keeps the member at its
	 * in-class default and the settings around it still load. The defaults
	 * live only in the member initializers above, so they cannot drift.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		archive::Optional(ar,
		                  CEREAL_NVP(r_pipeline), CEREAL_NVP(r_aa), CEREAL_NVP(r_ao),
		                  CEREAL_NVP(r_shadow), CEREAL_NVP(r_ssr),
		                  CEREAL_NVP(r_gamma), CEREAL_NVP(r_shadow_bias),
		                  CEREAL_NVP(r_ao_ksize), CEREAL_NVP(r_ao_radius),
		                  CEREAL_NVP(r_sample_pf), CEREAL_NVP(r_transparent),
		                  CEREAL_NVP(r_fxaa_subpix), CEREAL_NVP(r_fxaa_edge_threshold),
		                  CEREAL_NVP(r_fxaa_edge_threshold_min),
		                  CEREAL_NVP(r_sampling_mode),
		                  CEREAL_NVP(r_debug_draw),
		                  CEREAL_NVP(r_sampling_count));
	}

	// --- Query helpers ---

	/** @brief Returns true if any shadow algorithm is active. */
	bool RequiresShadow() const { return r_shadow != ShadowAlg::None; }

	/** @brief Returns true if screen-space reflections are enabled. */
	bool RequiresSSR() const { return r_ssr != SSRAlg::None; }

	/** @brief Returns true if SDF construction is needed for shadows or SSR. */
	bool RequiresSDF() const
	{
		return r_shadow == ShadowAlg::SDFSoftShadow ||
		       r_ssr == SSRAlg::SDFRayMarching ||
		       r_ssr == SSRAlg::SDFResolvedRayMarching;
	}

	/** @brief Returns true if FXAA is enabled. */
	bool RequiresFXAA() const { return r_aa == AAAlg::FXAA; }

	/** @brief Returns true if the viewport debug/gizmo overlay should be drawn. */
	bool RequiresDebugDraw() const { return r_debug_draw; }
};

/**
 * @brief The immutable defaults a null RenderConfig resolves to.
 *
 * The one place a missing config is answered: both context views that carry a
 * config pointer return this from their Settings(), so the fallback is never
 * restated as a ternary. A function-local static, so it *is* the in-class member
 * initializers rather than a second copy of them, and const, so sharing one
 * instance across frames and threads is safe.
 */
inline const RenderConfig& DefaultRenderConfig()
{
	static const RenderConfig kDefaults{};
	return kDefaults;
}

} // namespace neurus

