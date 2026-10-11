/**
 * @file Input.h
 * @brief Pure static translation helpers for converting raw input values to engine types.
 *
 * Stateless: raw positions, modifier/button/key integers in, engine values out.
 * No Qt dependency - callers unwrap Qt types before calling.
 */

#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace neurus {

/**
 * @brief Stateless static helper for translating raw input primitives.
 */
class Input
{
public:
	// -----------------------------------------------------------------------
	// Mouse button identifiers
	// -----------------------------------------------------------------------

	/** @brief Mouse button index. */
	enum MouseButton
	{
		Left = 0,   ///< Left mouse button
		Right = 1,  ///< Right mouse button
		Middle = 2  ///< Middle mouse button
	};

	// -----------------------------------------------------------------------
	// Translation helpers
	// -----------------------------------------------------------------------

	/**
	 * @brief Builds a glm::vec2 from raw coordinates.
	 */
	static glm::vec2 GetMousePos(float x, float y);

	// -----------------------------------------------------------------------
	// Modifier key flags (combinable bitmask)
	// -----------------------------------------------------------------------

	/** @brief Modifier key flags used in event structs and camera mode selection. */
	enum Modifiers
	{
		Mod_None  = 0x0,  ///< No modifier keys active.
		Mod_Shift = 0x1,  ///< Shift key(s) held.
		Mod_Ctrl  = 0x2,  ///< Control key(s) held.
		Mod_Alt   = 0x4   ///< Alt key(s) held.
	};

	/**
	 * @brief Translates a raw modifier integer into a Modifiers bitmask.
	 * @param qtMods Raw modifier value (e.g. static_cast<uint32_t>(Qt::KeyboardModifiers)).
	 */
	static Modifiers GetModifiers(uint32_t qtMods);

	/**
	 * @brief Maps a raw button integer to Input::MouseButton.
	 * @param qtBtn Raw button value (e.g. static_cast<uint32_t>(Qt::MouseButton)).
	 * @return Corresponding Input::MouseButton. Falls back to Left for unrecognised values.
	 */
	static MouseButton GetMouseButton(uint32_t qtBtn);

	// -----------------------------------------------------------------------
	// Keyboard identifiers
	// -----------------------------------------------------------------------

	/**
	 * @brief Keys the Editor reacts to, as an engine-side enum.
	 *
	 * Deliberately NOT the full keyboard: only keys some editor system consumes,
	 * so an unhandled key becomes Key_Unknown and is dropped at the boundary.
	 * The values are arbitrary and stable; they are not Qt codes.
	 */
	enum Key
	{
		Key_Unknown = 0,  ///< Not a key the editor handles; handlers must ignore it.

		Key_G,            ///< Modal transform: grab/move.
		Key_R,            ///< Modal transform: rotate.
		Key_S,            ///< Modal transform: scale.

		Key_X,            ///< Axis constraint X.
		Key_Y,            ///< Axis constraint Y.
		Key_Z,            ///< Axis constraint Z.
		Key_W,            ///< Axis constraint: screen space (drop X/Y/Z).

		Key_Escape,       ///< Cancel the active modal operation.
		Key_Return        ///< Confirm the active modal operation (Return and Enter both).
	};

	/**
	 * @brief Translates a raw key integer into Input::Key.
	 * @param qtKey Raw key value (e.g. static_cast<uint32_t>(event->key())).
	 * @return Corresponding Input::Key, or Key_Unknown when the editor has no use
	 *         for that key. Letter keys map irrespective of Shift/Caps, because Qt
	 *         reports the unshifted code for plain letters.
	 */
	static Key GetKey(uint32_t qtKey);
};

} // namespace neurus
