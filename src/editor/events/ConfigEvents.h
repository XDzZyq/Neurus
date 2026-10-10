#pragma once

#include "core/RenderConfig.h"

namespace neurus {

/** @brief Emitted when the render configuration is changed from the UI panel. */
struct RenderConfigChangedEvent { RenderConfig config; };

/**
 * @brief Emitted when a bounded render-config edit gesture begins (slider press).
 *
 * Marks the start of a continuous slider drag. RenderConfigController captures
 * the "before" config on this event and applies intermediate values live
 * without recording, so the whole drag collapses to one undo entry committed on
 * ConfigEditEnd.
 */
struct ConfigEditBegin {};

/**
 * @brief Emitted when a bounded render-config edit gesture ends (slider release).
 *
 * RenderConfigController records a single SetRenderConfigOp spanning the config
 * captured at ConfigEditBegin through the current config, if it actually changed.
 */
struct ConfigEditEnd {};

} // namespace neurus
