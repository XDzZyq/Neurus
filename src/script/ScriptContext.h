/**
 * @file ScriptContext.h
 * @brief Non-owning document view handed to one script run.
 *
 * A plain aggregate of non-owning pointers. Unlike a persistent context, it is
 * built fresh for one script run (`Editor::UpdateScriptContext`) and discarded
 * afterwards - there is no process-wide ScriptContext.
 *
 * The scene is the single source of truth: a script reads the active camera and
 * the selection from it live, so neither is snapshotted here. Content creation
 * is not part of this view either: a script asks for it through typed editor
 * events, so the context stays read-only. The header forward-declares `Scene`,
 * `RenderConfig` and `IResourceLookup` (never includes them) so including it
 * does not pull editor or renderer state into a translation unit.
 */

#pragma once

namespace neurus
{

class Scene;
class RenderConfig;
class IResourceLookup;

/**
 * @brief Non-owning view of the state one script may read.
 *
 * All pointers are borrowed; the caller guarantees they outlive the context.
 */
struct ScriptContext
{
	const Scene* scene = nullptr;                ///< Borrowed scene being read.
	const RenderConfig* config = nullptr;        ///< Borrowed render configuration.
	const IResourceLookup* resources = nullptr;  ///< Borrowed pool for name/UID lookup.
};

}  // namespace neurus
