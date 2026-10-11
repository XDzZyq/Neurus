/**
 * @file ControllerContext.h
 * @brief Bundle of the three controller-facing interfaces + editor singleton access.
 *
 * Controllers depend ONLY on this context: events through IEventQueue, pooled
 * objects through IResourceLookup, undoable operations through IOperationSink -
 * never on the concrete EventQueue, ResourceManager or OperationManager.
 *
 * It additionally carries providers for the Editor-owned singletons that are not
 * pooled objects: the current Scene (re-queried per use, because New/Load swaps
 * it), the live RenderConfig, and the viewport (its screen space AND its gizmo -
 * the viewport owns the gizmo, so there is no second provider for it).
 *
 * `viewport` is handed over mutable so Gizmo() is reachable; its own mutators
 * remain the Editor's alone. Every query a controller needs is const, so a
 * read-only handler should still bind `const EditorViewport*`.
 *
 * Lifetime: the Editor constructs the context once, before the controller list,
 * so it outlives the event subscriptions that capture it by reference.
 */

#pragma once

#include <functional>

#include "core/IResourceLookup.h"
#include "editor/events/IEventQueue.h"
#include "editor/operations/IOperationSink.h"

namespace neurus {

class Scene;
class RenderConfig;
class EditorViewport;

/**
 * @brief The three controller-facing interfaces plus editor singleton access.
 */
struct ControllerContext
{
	/** @brief Typed event dispatch: subscribe / enqueue / emitNow. */
	IEventQueue& events;

	/** @brief Read-only fetch/lookup of pooled UID objects by integer id. */
	IResourceLookup& resources;

	/** @brief Sink for recording undoable operations at mutation time. */
	IOperationSink& ops;

	/** @brief Returns the current Scene (re-queried per use; may swap on New/Load). */
	std::function<Scene*()> scene;

	/** @brief Returns the live Editor-owned RenderConfig to mutate. */
	std::function<RenderConfig*()> config;

	/**
	 * @brief Returns the Editor's viewport: its screen space and its gizmo.
	 *
	 * May return a viewport whose IsValid() is false (no camera, or zero size);
	 * callers must check rather than assume.
	 *
	 * Mutable only so that `Gizmo()` is reachable - see the file comment.
	 */
	std::function<EditorViewport*()> viewport;
};

} // namespace neurus
