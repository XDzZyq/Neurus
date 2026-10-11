/**
 * @file Serialize.h
 * @brief Version-tolerant cereal helpers: fields that may be absent from a file.
 *
 * Two builds of the app rarely agree on a field list, and cereal reports a
 * missing field by throwing, which unwinds past the whole component's Load -
 * so one unknown field can discard an entire scene. These helpers read such a
 * field on its own and let it keep its default:
 *
 *   - Optional()        one or more fields, each independent (the default).
 *   - LoadFresh()       a whole object read into a default-constructed
 *                       temporary, for a load into an object already in use.
 *   - OptionalBlock()   several fields all-or-nothing, returning a bool.
 *
 * For "absent" to mean "default", the object must start at its defaults: its
 * in-class member initializers are the only place a default is written, and
 * nothing in a load path restates it. Value fields, not identity: UID::o_id,
 * the base_class hooks, the resource pool, ID lists and operations stay
 * strict, because absence there means a broken reference.
 *
 * @note Works with name-keyed (text) archives only; the positional binary
 *       archives cannot report "no such name", so Optional() static_asserts
 *       against them rather than corrupting the stream.
 */

#pragma once

#include <cereal/cereal.hpp>

#include <cstddef>
#include <utility>

#include "core/Log.h"

namespace neurus::archive {
// Named `archive` rather than `serialize`: several headers declare free
// `neurus::serialize(Archive&, T&)` overloads for cereal to find by ADL, and
// a namespace of that name inside neurus would be a redefinition of those
// names.

/**
 * @brief Reads or writes one field that may be absent from the archive.
 *
 * Saving writes @p field and returns true. Loading reads it, and on absence
 * leaves the member exactly as it was and returns false - so the value the
 * caller ends up with is what the object was constructed with, and the
 * member's in-class initializer stays the single source of truth for its
 * default.
 *
 * @param ar     Archive being saved to or loaded from.
 * @param field  The field to transfer, normally a `CEREAL_NVP(x)` or
 *               `cereal::make_nvp("key", x)` wrapper so the name-keyed
 *               archives can look it up. Its name is logged when absent.
 * @return true when the field was transferred; false only on a load that
 *         found it missing.
 *
 * @note Either the whole field is in the file or none of it is: a compound
 *       field whose node exists but whose contents are incomplete is corrupt,
 *       not version drift, and is not what this helper is for.
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
			// info, not an error.
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
 * The usual entry point. Unlike OptionalBlock() the fields do not share a
 * fate: one name the file lacks does not stop the ones around it from
 * loading, so a renamed or removed field costs exactly itself.
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
 * For sites that read into an object the app is already using, where
 * Optional()'s "absence keeps the current value" would mean "absence keeps
 * whatever the running session had". The temporary is the point: every field
 * of @p live ends up either read from the archive or freshly constructed, so
 * the member initializers stay the only definition of the defaults.
 *
 * @param ar    Archive being loaded from (load-only: saving has nothing to
 *              default).
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
 * whatever defaults the block needs - which is why this returns a bool instead
 * of taking a fallback value: a block is often several fields that must be reset
 * together.
 *
 * Prefer Optional() unless the group genuinely is a unit. This form exists for
 * recognising a whole legacy format, and for fields where "absent" must stay
 * distinguishable from any value the field can hold.
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
 * exactly like the member, so it expands to a save of @p field, or a load
 * that leaves it at its in-class default when the file has no such field.
 * @p field must name a member directly, since CEREAL_NVP stringifies it as
 * the archive key; a member whose key differs needs the function form with
 * `cereal::make_nvp`.
 */
#define NEURUS_OPTIONAL(ar, field)                                                \
	((void)::neurus::archive::Optional((ar), CEREAL_NVP(field)))

// There is deliberately no `NEURUS_OPTIONAL_NVP(ar, field, fallback)`: a
// fallback argument is a second place a default can be written, free to
// disagree with the class. Absence now means "keep what the constructor set";
// a field that genuinely needs a load-time value different from its
// constructed one is a migration, written as an explicit `if (!Optional(...))`
// at the call site, where the reason can be stated.
