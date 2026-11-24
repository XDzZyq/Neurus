/**
 * @file test_input.cpp
 * @brief Unit tests for the Input static translation helpers.
 *
 * Verifies GetMousePos, GetModifiers, GetMouseButton and GetKey convert raw
 * values to engine types correctly. Qt types are unwrapped by the test code;
 * Input.hpp itself has zero Qt dependencies.
 *
 * GetKey is the narrowest of the four and the one with teeth: it is the only
 * place a Qt key code exists in the entire editor. Everything downstream — the
 * modal gizmo's G/R/S, its X/Y/Z/W constraints, Esc and Enter — compares against
 * Input::Key, so a wrong arm here silently rebinds a gesture rather than failing,
 * and an unmapped key must become Key_Unknown rather than leak a number.
 */

#include <gtest/gtest.h>

#include <QObject>   // Qt::KeyboardModifiers, Qt::MouseButton

#include "editor/Input.hpp"

using namespace neurus;

// ===========================================================================
// GetMousePos — float pair → glm::vec2
// ===========================================================================

TEST(InputTest, MousePositionPreservesCoordinates)
{
	for (const glm::vec2 position : { glm::vec2(100.5f, 200.25f), glm::vec2(0.0f), glm::vec2(-50.0f, -75.0f) })
		EXPECT_EQ(Input::GetMousePos(position.x, position.y), position) << position.x << ", " << position.y;
}

// ===========================================================================
// GetModifiers — uint32_t → Input::Modifiers bitmask
// ===========================================================================

TEST(InputTest, ModifierMaskTranslatesIndependentFlags)
{
	struct Case { Qt::KeyboardModifiers raw; int expected; };
	for (const Case row : { Case{Qt::NoModifier, Input::Mod_None},
	                        Case{Qt::ShiftModifier, Input::Mod_Shift},
	                        Case{Qt::ControlModifier, Input::Mod_Ctrl},
	                        Case{Qt::AltModifier, Input::Mod_Alt},
	                        Case{Qt::ControlModifier | Qt::ShiftModifier, Input::Mod_Ctrl | Input::Mod_Shift},
	                        Case{Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier,
	                             Input::Mod_Ctrl | Input::Mod_Shift | Input::Mod_Alt} })
		EXPECT_EQ(Input::GetModifiers(static_cast<uint32_t>(row.raw)), row.expected)
			<< "Qt modifier mask " << static_cast<int>(row.raw);
}

// ===========================================================================
// GetMouseButton — uint32_t → Input::MouseButton
// ===========================================================================

TEST(InputTest, MouseButtonsTranslateToEditorButtons)
{
	EXPECT_EQ(Input::GetMouseButton(static_cast<uint32_t>(Qt::LeftButton)), Input::MouseButton::Left);
	EXPECT_EQ(Input::GetMouseButton(static_cast<uint32_t>(Qt::RightButton)), Input::MouseButton::Right);
	EXPECT_EQ(Input::GetMouseButton(static_cast<uint32_t>(Qt::MiddleButton)), Input::MouseButton::Middle);
}

// ===========================================================================
// GetKey — uint32_t → Input::Key
// ===========================================================================

TEST(InputTest, GestureKeysTranslateToEditorActions)
{
	using Key = Input::Key;
	struct Case { Qt::Key raw; Key expected; };
	for (const Case row : { Case{Qt::Key_G, Key::Key_G}, Case{Qt::Key_R, Key::Key_R},
	                        Case{Qt::Key_S, Key::Key_S}, Case{Qt::Key_X, Key::Key_X},
	                        Case{Qt::Key_Y, Key::Key_Y}, Case{Qt::Key_Z, Key::Key_Z},
	                        Case{Qt::Key_W, Key::Key_W}, Case{Qt::Key_Escape, Key::Key_Escape},
	                        Case{Qt::Key_Return, Key::Key_Return}, Case{Qt::Key_Enter, Key::Key_Return} })
		EXPECT_EQ(Input::GetKey(row.raw), row.expected) << "Qt key " << static_cast<int>(row.raw);
}

/**
 * @test Anything unmapped becomes Key_Unknown, never a passed-through number.
 *
 * The event queue carries Input::Key and nothing else, so a leaked Qt code could
 * collide with a real enumerator — Key_G is 1, and any small integer would match.
 */
TEST(InputTest, GetKey_UnmappedKeysAreUnknown)
{
	for (const Qt::Key key : {Qt::Key_A, Qt::Key_Q, Qt::Key_Space, Qt::Key_Tab,
	                          Qt::Key_F1, Qt::Key_Delete, Qt::Key_0})
		EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(key)), Input::Key::Key_Unknown)
			<< "Qt key " << static_cast<int>(key) << " is not one the editor handles";
	static_assert(static_cast<int>(Input::Key::Key_Unknown) == 0);
}
