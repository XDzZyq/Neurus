/**
 * @file ScriptContext.h
 * @brief Non-owning document and creation view handed to one script run.
 *
 * A plain aggregate of non-owning pointers. Unlike a persistent context, it is
 * built fresh for one script run (`Editor::UpdateScriptContext`) and discarded
 * afterwards - there is no process-wide ScriptContext.
 *
 * The scene is the single source of truth: a script reads the active camera and
 * the selection from it live, so neither is snapshotted here. The header
 * forward-declares `Scene`, `RenderConfig`, `IResourceLookup` and
 * `IScriptEditor` (never includes them) so including it does not pull editor or
 * renderer state into a translation unit.
 */

#pragma once

namespace neurus
{

class Scene;
class RenderConfig;
class IResourceLookup;
class IScriptEditor;
class IScriptHost;

/**
 * @brief Non-owning view of the state and creation operations one script may use.
 *
 * All pointers are borrowed; the caller guarantees they outlive the context.
 */
struct ScriptContext
{
	const Scene* scene = nullptr;                ///< Borrowed scene being edited.
	const RenderConfig* config = nullptr;        ///< Borrowed render configuration.
	const IResourceLookup* resources = nullptr;  ///< Borrowed pool for name/UID lookup.
	IScriptEditor* editor = nullptr;             ///< Borrowed content-creation surface.
	IScriptHost* host = nullptr;                 ///< Borrowed application save/snapshot surface.
};

}  // namespace neurus
