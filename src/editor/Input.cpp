/**
 * @file Input.cpp
 * @brief Implementation of Input static translation helpers.
 *
 * Qt types are unwrapped by callers — Input.cpp only deals with raw C++ types.
 * The internal conversions match Qt::KeyboardModifier / Qt::MouseButton values,
 * but no Qt headers are exposed through Input.h.
 */

#include "editor/Input.h"

#include <QObject>   // Qt::KeyboardModifier, Qt::MouseButton

namespace neurus {

// ---------------------------------------------------------------------------
// GetMousePos — float pair → glm::vec2
// ---------------------------------------------------------------------------

glm::vec2 Input::GetMousePos(float x, float y)
{
	return glm::vec2(x, y);
}

// ---------------------------------------------------------------------------
// GetModifiers — uint32_t → Modifiers bitmask
// ---------------------------------------------------------------------------

Input::Modifiers Input::GetModifiers(uint32_t qtMods)
{
	auto mods = static_cast<Qt::KeyboardModifiers>(qtMods);

	int result = Mod_None;
	if (mods & Qt::ShiftModifier)   result |= Mod_Shift;
	if (mods & Qt::ControlModifier) result |= Mod_Ctrl;
	if (mods & Qt::AltModifier)     result |= Mod_Alt;
	return static_cast<Modifiers>(result);
}

// ---------------------------------------------------------------------------
// GetMouseButton — uint32_t → MouseButton
// ---------------------------------------------------------------------------

Input::MouseButton Input::GetMouseButton(uint32_t qtBtn)
{
	switch (static_cast<Qt::MouseButton>(qtBtn))
	{
	case Qt::LeftButton:   return MouseButton::Left;
	case Qt::RightButton:  return MouseButton::Right;
	case Qt::MiddleButton: return MouseButton::Middle;
	default:               return MouseButton::Left;  // fallback
	}
}

// ---------------------------------------------------------------------------
// GetKey — uint32_t → Key
// ---------------------------------------------------------------------------

Input::Key Input::GetKey(uint32_t qtKey)
{
	switch (static_cast<Qt::Key>(qtKey))
	{
	case Qt::Key_G:      return Key::Key_G;
	case Qt::Key_R:      return Key::Key_R;
	case Qt::Key_S:      return Key::Key_S;

	case Qt::Key_X:      return Key::Key_X;
	case Qt::Key_Y:      return Key::Key_Y;
	case Qt::Key_Z:      return Key::Key_Z;

	case Qt::Key_Escape: return Key::Key_Escape;

	// Return and Enter are physically distinct keys (main block vs. numpad) that
	// mean the same thing to every editor operation, so they collapse here rather
	// than forcing each handler to test both.
	case Qt::Key_Return:
	case Qt::Key_Enter:  return Key::Key_Return;

	// Everything else is deliberately dropped at the boundary: the event queue
	// carries Input::Key, never a raw Qt code, so an unmapped key cannot leak
	// through as a number some handler might compare against by accident.
	default:             return Key::Key_Unknown;
	}
}

} // namespace neurus
