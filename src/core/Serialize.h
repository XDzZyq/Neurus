/**
 * @file Serialize.h
 * @brief Backward-compatible cereal helpers for fields added after a format shipped.
 *
 * A project file written before a field existed simply does not contain it, and
 * cereal reports that by throwing `cereal::Exception` from the read. Left
 * unhandled the throw aborts the whole component's Load, so one new setting
 * discards every setting around it. The fix is always the same three lines —
 * save unconditionally, load inside a try, fall back on the catch — which this
 * header collapses into one call so the pattern cannot drift between sites.
 *
 * Use it for fields **appended** to an existing block. Order still matters:
 * cereal's binary and portable-binary archives are positional, so an optional
 * field must be written last, and once a file exists with it, its position is
 * fixed forever.
 */

#pragma once

#include <cereal/cereal.hpp>

#include <utility>

namespace neurus::archive {
// Named `archive` rather than `serialize`: several headers declare free
// `neurus::serialize(Archive&, T&)` overloads for cereal to find by ADL (see
// render/shaders/ShaderStructSerialize.h), and a namespace of that name inside
// neurus would be a redefinition of those names.

/**
 * @brief Reads or writes fields that may be absent from an older archive.
 *
 * Saving writes @p fields and returns true. Loading reads them and returns
 * false if the archive does not contain them, leaving the caller to install
 * whatever defaults the block needs — which is why this returns a bool instead
 * of taking a fallback value: a block is often several fields that must be reset
 * together (see Scene::serialize).
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
 * @brief Transfers one optional field, resetting it to @p fallback when absent.
 *
 * The single-field shape of OptionalBlock(), which is the common case:
 * @code
 *   NEURUS_OPTIONAL_NVP(ar, r_debug_draw, true);
 * @endcode
 * expands to a save of `r_debug_draw`, or a load that falls back to `true` when
 * the archive predates the field. @p field must name a member directly, since
 * CEREAL_NVP stringifies it as the archive key.
 */
#define NEURUS_OPTIONAL_NVP(ar, field, fallback)                                  \
	do                                                                            \
	{                                                                             \
		if (!::neurus::archive::OptionalBlock((ar), CEREAL_NVP(field)))          \
		{                                                                         \
			(field) = (fallback);                                                 \
		}                                                                         \
	} while (false)
