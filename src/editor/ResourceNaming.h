/**
 * @file ResourceNaming.h
 * @brief Registers a pooled object's display name with the resource registry.
 */

#pragma once

#include "core/IResourceLookup.h"
#include "scene/ObjectID.h"

namespace neurus
{

/**
 * @brief Registers @p obj's o_name and mirrors the unique result back.
 *
 * The registry owns uniqueness, so the requested name may gain a ".001" suffix.
 * Writing the returned name back into o_name is what keeps the Outliner, the
 * project file and name lookups agreeing with each other.
 *
 * @param lookup Registry the object is pooled in.
 * @param obj Pooled object whose o_name is registered and mirrored.
 */
inline void MirrorResourceName(IResourceLookup& lookup, ObjectID& obj)
{
	obj.o_name = lookup.SetName(obj.GetObjectID(), obj.o_name);
}

} // namespace neurus
