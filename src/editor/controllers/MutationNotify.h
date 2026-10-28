/**
 * @file MutationNotify.h
 * @brief The single answer to "what does a scene mutation imply?".
 *
 * Mutating a scene object is never just the write. Which consequences apply is a
 * function of two things only - the object's type and whether the edit is final -
 * so it is answered here once, for every controller, instead of at each site.
 *
 * Two tiers: `ObjectMutated` is the committed tier (a panel edit, an add, a
 * confirmed gesture) and marks the project dirty. `ObjectPreviewed` is the
 * transient tier (a frame of a live drag, which the user may abandon) and
 * deliberately does not, because SceneModified is one-way: a cancelled gesture
 * must leave no trace. A preview buys the cheap single-struct light update; a
 * committed edit pays for the authoritative full-dict rebuild, since the dict
 * filters by shader variant and visibility.
 */

#pragma once

#include "editor/events/EditorEvents.h"
#include "editor/events/IEventQueue.h"

#include "scene/ObjectID.h"

namespace neurus::notify {

/** @brief Something visible changed: restart temporal accumulation and redraw. */
inline void Repaint(IEventQueue& events)
{
	events.Emit(RenderResetEvent{});
}

/** @brief A committed scene change with no GPU side effects: dirty + repaint. */
inline void Mutated(IEventQueue& events)
{
	events.Emit(SceneModified{});
	Repaint(events);
}

/** @brief A committed change to one light's SSBO struct (power, radius, cone). */
inline void LightStructChanged(int lightUid, IEventQueue& events)
{
	events.Emit(LightGpuChanged{lightUid});
	Mutated(events);
}

/** @brief A committed change that invalidates the whole light dict. */
inline void LightingRebuilt(IEventQueue& events)
{
	events.Emit(LightingRebuild{});
	Mutated(events);
}

/**
 * @brief Committed tier: @p obj was edited for keeps. Dirty + type-correct GPU sync.
 *
 * A light's transform, visibility and membership all reach the light dict rather
 * than one struct, because the upload filters by shader variant and visibility -
 * the dict, not the entry, is what the edit changed.
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
 * No op is recorded here either: a bounded gesture submits exactly one op when
 * it is confirmed.
 */
inline void ObjectPreviewed(const ObjectID& obj, IEventQueue& events)
{
	if (obj.IsLight())
		events.Emit(LightGpuChanged{obj.GetObjectID()});
	Repaint(events);
}

} // namespace neurus::notify
