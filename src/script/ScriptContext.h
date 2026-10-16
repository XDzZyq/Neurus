/**
 * @file ScriptContext.h
 * @brief Read-only view of the scene and render config handed to a script run.
 *
 * A plain aggregate of non-owning pointers. Unlike a persistent context, it is
 * built fresh for one script run (`Editor::UpdateScriptContext`) and discarded
 * afterwards - there is no process-wide ScriptContext.
 *
 * The scene is the single source of truth: a script reads the active camera and
 * the selection from it live, so neither is snapshotted here. The header
 * forward-declares `Scene` and `RenderConfig` (never includes them) so including
 * it does not pull editor or renderer state into a translation unit.
 */

#pragma once

namespace neurus
{

class Scene;
class RenderConfig;

/**
 * @brief Non-owning, read-only view of the state a script may read.
 *
 * All pointers are borrowed; the caller guarantees they outlive the context.
 */
struct ScriptContext
{
	const Scene* scene = nullptr;          ///< Borrowed scene being edited.
	const RenderConfig* config = nullptr;  ///< Borrowed render configuration.
};

}  // namespace neurus
