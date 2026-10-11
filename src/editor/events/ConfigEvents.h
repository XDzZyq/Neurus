#pragma once

#include "core/RenderConfig.h"

namespace neurus {

/** @brief Emitted when the render configuration is changed from the UI panel. */
struct RenderConfigChangedEvent { RenderConfig config; };

/**
 * @brief Emitted when a bounded render-config edit gesture begins (slider press).
 *
 * The controller captures the "before" config and applies intermediate values
 * live without recording, so the whole drag collapses to one undo entry
 * committed on ConfigEditEnd.
 */
struct ConfigEditBegin {};

/**
 * @brief Emitted when a bounded render-config edit gesture ends (slider release).
 *
 * The controller records a single op spanning the captured config through the
 * current config, if it changed.
 */
struct ConfigEditEnd {};

} // namespace neurus
