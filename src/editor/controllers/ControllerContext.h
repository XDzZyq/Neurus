/**
 * @file ControllerContext.h
 * @brief Bundle of the three controller-facing interfaces + editor singleton access.
 *
 * Controllers depend ONLY on this context: they subscribe/enqueue/emitNow
 * events through IEventQueue, resolve pooled objects by integer id through
 * IResourceLookup, and record undoable operations through IOperationSink —
 * never on the concrete EventQueue, ResourceManager, or OperationManager.
 *
 * The context additionally carries providers for the Editor-owned singletons
 * that are NOT pooled UID objects:
 *   - scene:    the current Scene (re-queried per use, because New/Load swaps it)
 *   - config:   the live RenderConfig to mutate (stable Editor member)
 *   - viewport: the viewport composition root — its screen space AND its gizmo
 *
 * There is exactly one handle per Editor-owned thing, and the viewport is one
 * thing. The modal transform state machine is reached as `viewport()->Gizmo()`,
 * never as a provider of its own: the viewport owns it, so a second provider would
 * be a second name for the same object, free to be wired to a different one.
 *
 * That is why `viewport` is handed over mutable. The viewport's own mutators —
 * extent, camera, cursor, overlay dirty flags — remain the Editor's alone: they are
 * a per-frame job driven by Edit() and by the Application's resize, and a controller
 * writing them would desync the projection from the extent the renderer is actually
 * using. That contract is documentation here rather than a compiler check, which is
 * the price of the single handle and worth paying: a `const` that has to be routed
 * around by handing out a second pointer to the same object was never enforcing
 * anything. Every *query* a controller needs — Project, RayThrough,
 * PixelsPerWorldUnit, Cursor — is const, so a read-only handler should still bind
 * `const EditorViewport*` and let the compiler hold it to that locally.
 *
 * Ownership/lifetime rules:
 * - The context is constructed by the Editor once and outlives the
 *   controllers' event subscriptions (declared before the controller list).
 * - Controllers MUST NOT store the context or any of its members; handler
 *   lambdas may capture it by reference (it outlives the EventQueue, whose
 *   stored lambdas reference it).
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
	 * callers must check rather than assume, because every projection result
	 * carries its own validity flag for exactly that reason.
	 *
	 * Mutable only so that `Gizmo()` is reachable — see the file comment.
	 */
	std::function<EditorViewport*()> viewport;
};

} // namespace neurus
