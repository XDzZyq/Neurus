/**
 * @file ScriptContext.hpp
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

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace neurus
{

class Scene;
class RenderConfig;
class IResourceLookup;
class RenderGraphData;
struct CPUGraphDiagnostic;

/// @brief The current effective graph and the identity of its optional project resource.
struct ScriptGraphView
{
	const RenderGraphData* topology = nullptr;
	std::optional<int> uid;
	std::uint64_t projectEpoch = 0;
};

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
	/// Re-query graph topology and identity after creation or undo in the same run.
	std::function<ScriptGraphView()> graphProvider;
	/// Named validation errors for the graph currently visible to the script.
	std::function<std::vector<CPUGraphDiagnostic>()> graphDiagnostics;
	/// Re-query optional config after writes within the same script run.
	std::function<const RenderConfig*()> configProvider;
};

}  // namespace neurus
