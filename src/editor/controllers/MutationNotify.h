/**
 * @file MutationNotify.h
 * @brief The single answer to "what does a scene mutation imply?".
 *
 * Mutating a scene object is never just the write. Something has to reset temporal
 * accumulation, something has to refresh the light SSBO the write did not touch, and
 * something has to mark the project unsaved. Which of those apply is a function of
 * two things only — the object's type, and whether the edit is final — so it is
 * answered here once, for every controller, instead of at each mutation site.
 *
 * It lived at each site before, and that is exactly how it drifted: the same
 * `if (GO_LIGHT || GO_POLYLIGHT)` branch was written out five times in
 * SceneController.cpp alone, next to a private IsLightObject() that none of the five
 * called, and a sixth copy in TransformGizmoController.cpp. Six copies of one policy
 * is six places to forget when a new object type needs GPU sync on transform.
 *
 * ## Two tiers: committed and transient
 *
 * `ObjectMutated` is the committed tier — a panel edit, an add, a confirmed gesture.
 * It marks the project dirty.
 *
 * `ObjectPreviewed` is the transient tier — a frame of a live drag, which the user
 * may still abandon. It deliberately does **not** mark the project dirty, because
 * `SceneModified` is one-way: nothing un-dirties a project, so a cancelled gesture
 * that had emitted it would leave the document needing a save for a change that no
 * longer exists. The cancel path is itself a preview (it restores the snapshot), so
 * it routes here too, and a whole G-Esc round trip leaves no trace at all.
 *
 * The tiers also differ in how much light work they ask for, and that asymmetry is
 * deliberate rather than an oversight: a preview runs per mouse-move and buys the
 * cheap single-struct update (`LightGpuChanged`), while a committed edit pays for the
 * authoritative full-dict rebuild (`LightingRebuild`) — the dict filters by shader
 * variant and visibility, so only it is correct once the edit is permanent.
 *
 * ## Scope
 *
 * This is mutation policy, not a general event façade. A camera orbit and a render
 * setting also enqueue `RenderResetEvent`, but they mutate no scene object and have
 * no tier; they keep enqueueing it directly. `Repaint` is here because both tiers
 * compose it, and for the overlay-only case of a gizmo guide that appeared or
 * vanished without anything being mutated.
 */

#pragma once

#include "editor/events/EditorEvents.h"
#include "editor/events/IEventQueue.h"

#include "scene/ObjectID.h"

namespace neurus::notify {

/** @brief Something visible changed: restart temporal accumulation and redraw. */
inline void Repaint(IEventQueue& events)
{
	events.enqueue(RenderResetEvent{});
}

/** @brief A committed scene change with no GPU side effects: dirty + repaint. */
inline void Mutated(IEventQueue& events)
{
	events.enqueue(SceneModified{});
	Repaint(events);
}

/** @brief A committed change to one light's SSBO struct (power, radius, cone). */
inline void LightStructChanged(int lightUid, IEventQueue& events)
{
	events.enqueue(LightGpuChanged{lightUid});
	Mutated(events);
}

/** @brief A committed change that invalidates the whole light dict. */
inline void LightingRebuilt(IEventQueue& events)
{
	events.enqueue(LightingRebuild{});
	Mutated(events);
}

/**
 * @brief Committed tier: @p obj was edited for keeps. Dirty + type-correct GPU sync.
 *
 * A light's transform, visibility and membership all reach the SSBO dict rather than
 * one struct, because UploadLighting filters by shader variant and visibility while
 * walking Scene::light_list — so the dict, not the entry, is what the edit changed.
 */
inline void ObjectMutated(const ObjectID& obj, IEventQueue& events)
{
	if (obj.IsLight())
		LightingRebuilt(events);
	else
		Mutated(events);
}

/**
 * @brief Transient tier: @p obj is mid-gesture. GPU sync + repaint, never dirty.
 *
 * The cheap single-light path on purpose — see the file comment. No op is recorded
 * here either: a bounded gesture submits exactly one op when it is confirmed.
 */
inline void ObjectPreviewed(const ObjectID& obj, IEventQueue& events)
{
	if (obj.IsLight())
		events.enqueue(LightGpuChanged{obj.GetObjectID()});
	Repaint(events);
}

} // namespace neurus::notify
