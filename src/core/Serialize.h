/**
 * @file Serialize.h
 * @brief Version-tolerant cereal helpers: fields that may be absent from a file.
 *
 * Two builds of the app rarely agree on a field list. An older build opening a
 * newer project, or a newer build opening an older one, hits the same wall: a
 * name the reader asks for is not in the file, cereal reports that by throwing
 * `cereal::Exception`, and the throw unwinds past the whole component's Load.
 * Because the catch blocks are deliberately coarse — `ResourceComponent::Load`
 * calls `ResourceManager::Clear()`, `ConfigComponent::Load` assigns a fresh
 * `RenderConfig` — one unknown field can discard an entire scene or every
 * render setting around it.
 *
 * The cure is to read such a field on its own and let it keep its default when
 * the file has nothing to say about it. This header holds the three shapes of
 * that, so the pattern cannot drift between call sites:
 *
 *   - Optional()        one or more fields, each independent (the default).
 *   - LoadFresh()       a whole object read into a default-constructed temporary
 *                       and assigned over a live one, for the two sites that
 *                       load into an object that is already in use.
 *   - OptionalBlock()   several fields all-or-nothing, returning a bool, for
 *                       detecting a whole legacy block (see Scene::serialize).
 *
 * ## One definition of every default
 *
 * A field's default is written **once**, in its in-class member initializer (or
 * the type's default constructor, for `o_type` and `o_name`). Nothing in a load
 * path restates it, and that is enforced rather than asked for:
 *
 *   - Optional() on absence *leaves the member alone*. There is no fallback
 *     parameter to pass, so there is no second place a default could be spelled
 *     — a helper that took one was deleted for exactly that reason.
 *   - That only holds if the object starts at its defaults, which is free for
 *     anything cereal constructs (every pooled scene object) but not for a load
 *     into a live object. The two such sites — `ConfigComponent::Load` and
 *     `Preferences::Load` — do not reset by hand; they read into a
 *     freshly-constructed instance and assign it over the live one, so the
 *     defaults come from that construction. LoadFresh() is that move for a whole
 *     object; `Preferences::Load` does it by declaring a default `Preferences`.
 *   - `test/asset/test_serialize_optional.cpp` loads a near-empty archive and
 *     compares the result field-by-field against a default-constructed object,
 *     which fails if the two ever disagree.
 *
 * ## What this does and does not buy
 *
 * A field the reader never asks for is already harmless: cereal's JSON input
 * archive looks members up by name and `finishNode()` never checks that an
 * object's members were all consumed, so a *newer* file's extra fields and
 * extra top-level components are skipped silently. Nothing here is needed for
 * that case. What these helpers add is tolerance for a field that was
 * **renamed, moved or removed** between the two builds — in either direction.
 *
 * Two version-drift hazards are out of scope and remain fatal to the block that
 * hits them (see data-resource.instructions.md):
 *   - a pooled object type this build has not registered, which still throws
 *     out of `ResourceManager::serialize` and clears the whole pool;
 *   - an enum value outside this build's range, which loads as-is.
 *
 * ## Which fields may be optional
 *
 * Value fields, not identity. `UID::o_id`, the `cereal::base_class<>` hooks,
 * the resource pool itself, `Scene`'s ID lists, a data resource's path and
 * every undo/redo Operation stay strict: absence there means a broken
 * reference or a replay that would corrupt the scene, which must surface
 * instead of being papered over.
 *
 * ## Name-keyed archives only
 *
 * Per-field optionality needs an archive that can report "no such name" without
 * losing its place. Every archive in this project is `cereal::JSONInputArchive`
 * (asset/Project.h, app/Preferences.cpp), whose `search()` scans the current
 * object from its first member, assigns the cursor only on a hit, and clears
 * its pending name before it can throw — so a failed lookup leaves the cursor
 * and the node stack untouched and **field order does not matter**. The
 * positional binary archives cannot do that, so Optional() static_asserts
 * against them rather than corrupting a stream.
 */

#pragma once

#include <cereal/cereal.hpp>

#include <cstddef>
#include <utility>

#include "core/Log.h"

namespace neurus::archive {
// Named `archive` rather than `serialize`: several headers declare free
// `neurus::serialize(Archive&, T&)` overloads for cereal to find by ADL (see
// render/shaders/ShaderStructSerialize.h), and a namespace of that name inside
// neurus would be a redefinition of those names.

/**
 * @brief Reads or writes one field that may be absent from the archive.
 *
 * Saving writes @p field and returns true. Loading reads it, and on absence
 * leaves the member exactly as it was and returns false — so the value the
 * caller ends up with is whatever the object was constructed with. That is the
 * point: the member's in-class initializer stays the single source of truth for
 * its default, with no literal duplicated into the load path.
 *
 * The caller therefore owes one thing: the object must start at its defaults.
 * True for free of anything cereal constructs (every pooled scene object), and
 * arranged explicitly where a load writes into a live object — see
 * `ConfigComponent::Load`, which assigns `RenderConfig{}` before reading.
 *
 * @param ar     Archive being saved to or loaded from.
 * @param field  The field to transfer, normally a `CEREAL_NVP(x)` or
 *               `cereal::make_nvp("key", x)` wrapper so the name-keyed archives
 *               can look it up. Its name is logged when it turns out absent.
 * @return true when the field was transferred; false only on a load that found
 *         it missing.
 *
 * @note Either the whole field is in the file or none of it is. A compound
 *       field (a glm::vec3, a vector) whose node exists but whose *contents*
 *       are incomplete throws from inside the node, which unwinds past the
 *       archive's matching `finishNode()` and leaves the cursor somewhere
 *       unhelpful. cereal never writes a node like that; a file that contains
 *       one is corrupt rather than merely a different version, and that is not
 *       what this helper is for.
 */
template<class Archive, class Field>
bool OptionalField(Archive& ar, Field&& field)
{
	if constexpr (Archive::is_saving::value)
	{
		ar(std::forward<Field>(field));
		return true;
	}
	else
	{
		static_assert(cereal::traits::is_text_archive<Archive>::value,
		              "neurus::archive::Optional needs a name-keyed (text) archive: "
		              "a positional archive cannot report an absent field without "
		              "desynchronising the stream.");
		try
		{
			ar(std::forward<Field>(field));
			return true;
		}
		catch (const cereal::Exception&)
		{
			// Expected whenever two builds disagree on a field list, so this is
			// info, not an error — same level as the legacy-file paths in
			// HistoryComponent::Load and EditorComponent::Load.
			if constexpr (requires { field.name; })
			{
				NEURUS_LOG("[archive] field '" << field.name
				           << "' absent - kept its default");
			}
			return false;
		}
	}
}

/**
 * @brief Transfers each field independently, keeping the default of any absent one.
 *
 * The usual entry point. Unlike OptionalBlock() the fields do not share a fate:
 * one name the file lacks does not stop the ones around it from loading, which
 * is what makes a renamed or removed field cost exactly itself.
 *
 * @code
 *   archive::Optional(ar, CEREAL_NVP(light_power), CEREAL_NVP(light_color),
 *                         cereal::make_nvp("m_radius", light_radius));
 * @endcode
 *
 * @return How many fields were absent (0 on any save).
 */
template<class Archive, class... Fields>
std::size_t Optional(Archive& ar, Fields&&... fields)
{
	std::size_t absent = 0;
	auto one = [&](auto&& field) {
		if (!OptionalField(ar, std::forward<decltype(field)>(field)))
			++absent;
	};
	// Comma fold: left-to-right, so saving writes the fields in the order given.
	(one(std::forward<Fields>(fields)), ...);
	return absent;
}

/**
 * @brief Loads one named object into a default-constructed temporary, then
 *        assigns it over @p live.
 *
 * For the sites that read into an object the app is already using, where
 * Optional()'s "absence keeps the current value" would otherwise mean "absence
 * keeps whatever the running session had" — a default that depends on when the
 * user opened the file, which is no default at all.
 *
 * The temporary is the whole point: every field of @p live ends up either read
 * from the archive or freshly constructed, so the member initializers stay the
 * only definition of the defaults and no call site has to remember to reset.
 *
 * @code
 *   archive::LoadFresh(ar, "proj_config", *m_config);
 * @endcode
 *
 * @param ar    Archive being loaded from (load-only: saving has nothing to
 *              default, and `ar(cereal::make_nvp(key, live))` is the save).
 * @param key   Archive key holding the object.
 * @param live  Object to overwrite. Must be default-constructible and
 *              assignable.
 * @return true when the key was found. On false @p live has been reset to a
 *         default-constructed value, which is what an absent node means.
 */
template<class Archive, class T>
bool LoadFresh(Archive& ar, const char* key, T& live)
{
	static_assert(Archive::is_loading::value,
	              "neurus::archive::LoadFresh is a load-side helper; save with "
	              "ar(cereal::make_nvp(key, live)).");
	T fresh{};
	const bool found = OptionalField(ar, cereal::make_nvp(key, fresh));
	live = std::move(fresh);
	return found;
}

/**
 * @brief Reads or writes fields that must all be present or all be absent.
 *
 * Saving writes @p fields and returns true. Loading reads them and returns
 * false if the archive does not contain them, leaving the caller to install
 * whatever defaults the block needs — which is why this returns a bool instead
 * of taking a fallback value: a block is often several fields that must be reset
 * together (see Scene::serialize).
 *
 * Prefer Optional() unless the group genuinely is a unit. This form exists for
 * recognising a whole legacy *format* — Scene's pre-pool ID lists, its selection
 * block — and for fields where "absent" must stay distinguishable from any value
 * the field can hold (Scene's three-state `activeCamUid`).
 *
 * @param ar      Archive being saved to or loaded from.
 * @param fields  Fields to transfer, normally `CEREAL_NVP(x)` wrappers so the
 *                name-based archives can look them up.
 * @return true when the fields were transferred; false only on a load that
 *         found them missing.
 *
 * @note The archive stays usable after a false return: a name-based archive
 *       leaves its cursor untouched when a lookup fails, so a following
 *       OptionalBlock() still resolves against the right node.
 */
template<class Archive, class... Fields>
bool OptionalBlock(Archive& ar, Fields&&... fields)
{
	if constexpr (Archive::is_saving::value)
	{
		ar(std::forward<Fields>(fields)...);
		return true;
	}
	else
	{
		try
		{
			ar(std::forward<Fields>(fields)...);
			return true;
		}
		catch (const cereal::Exception&)
		{
			return false;
		}
	}
}

} // namespace neurus::archive

/**
 * @brief Transfers one optional field by member name.
 *
 * Sugar over Optional() for the common case where the archive key is spelled
 * exactly like the member:
 * @code
 *   NEURUS_OPTIONAL(ar, r_debug_draw);
 * @endcode
 * expands to a save of `r_debug_draw`, or a load that leaves it at its in-class
 * default when the file has no such field. @p field must name a member
 * directly, since CEREAL_NVP stringifies it as the archive key; a member whose
 * key differs needs the function form with `cereal::make_nvp`.
 */
#define NEURUS_OPTIONAL(ar, field)                                                \
	((void)::neurus::archive::Optional((ar), CEREAL_NVP(field)))

// There is deliberately no `NEURUS_OPTIONAL_NVP(ar, field, fallback)`. It used
// to exist, and it was the one construct in the codebase that let a default be
// written a second time — `RenderConfig::r_debug_draw` carried `= true` in the
// class and `, true` in the loader, free to disagree. Absence now means "keep
// what the constructor set", so the fallback argument has nothing to say. A
// field that genuinely needs a load-time value *different* from its constructed
// one is a migration, not a default: write it as an explicit `if (!Optional(...))`
// at the call site, where the reason can be stated.
