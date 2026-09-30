/**
 * @file test_input.cpp
 * @brief Unit tests for the Input static translation helpers.
 *
 * Verifies GetMousePos, GetModifiers, GetMouseButton and GetKey convert raw
 * values to engine types correctly. Qt types are unwrapped by the test code;
 * Input.h itself has zero Qt dependencies.
 *
 * GetKey is the narrowest of the four and the one with teeth: it is the only
 * place a Qt key code exists in the entire editor. Everything downstream — the
 * modal gizmo's G/R/S, its X/Y/Z/W constraints, Esc and Enter — compares against
 * Input::Key, so a wrong arm here silently rebinds a gesture rather than failing,
 * and an unmapped key must become Key_Unknown rather than leak a number.
 */

#include <gtest/gtest.h>

#include <QObject>   // Qt::KeyboardModifiers, Qt::MouseButton
#include <QPoint>    // QPointF

#include "editor/Input.h"

using namespace neurus;

// ===========================================================================
// GetMousePos — float pair → glm::vec2
// ===========================================================================

TEST(InputTest, GetMousePos_ConvertsCorrectly)
{
	const glm::vec2 v = Input::GetMousePos(100.5f, 200.25f);

	EXPECT_FLOAT_EQ(v.x, 100.5f);
	EXPECT_FLOAT_EQ(v.y, 200.25f);
}

TEST(InputTest, GetMousePos_OriginReturnsZero)
{
	const glm::vec2 v = Input::GetMousePos(0.0f, 0.0f);

	EXPECT_FLOAT_EQ(v.x, 0.0f);
	EXPECT_FLOAT_EQ(v.y, 0.0f);
}

TEST(InputTest, GetMousePos_NegativeValues)
{
	const glm::vec2 v = Input::GetMousePos(-50.0f, -75.0f);

	EXPECT_FLOAT_EQ(v.x, -50.0f);
	EXPECT_FLOAT_EQ(v.y, -75.0f);
}

// ===========================================================================
// GetModifiers — uint32_t → Input::Modifiers bitmask
// ===========================================================================

TEST(InputTest, GetModifiers_NoModifiersReturnsNone)
{
	const auto mods = Input::GetModifiers(static_cast<uint32_t>(Qt::NoModifier));
	EXPECT_EQ(mods, Input::Mod_None);
}

TEST(InputTest, GetModifiers_ShiftReturnsShiftFlag)
{
	const auto mods = Input::GetModifiers(static_cast<uint32_t>(Qt::ShiftModifier));
	EXPECT_EQ(mods, Input::Mod_Shift);
}

TEST(InputTest, GetModifiers_CtrlReturnsCtrlFlag)
{
	const auto mods = Input::GetModifiers(static_cast<uint32_t>(Qt::ControlModifier));
	EXPECT_EQ(mods, Input::Mod_Ctrl);
}

TEST(InputTest, GetModifiers_AltReturnsAltFlag)
{
	const auto mods = Input::GetModifiers(static_cast<uint32_t>(Qt::AltModifier));
	EXPECT_EQ(mods, Input::Mod_Alt);
}

TEST(InputTest, GetModifiers_CombinedModifiers)
{
	const auto mods = Input::GetModifiers(
		static_cast<uint32_t>(Qt::ShiftModifier | Qt::ControlModifier));
	EXPECT_EQ(static_cast<int>(mods), Input::Mod_Shift | Input::Mod_Ctrl);
	EXPECT_TRUE(mods & Input::Mod_Shift);
	EXPECT_TRUE(mods & Input::Mod_Ctrl);
	EXPECT_FALSE(mods & Input::Mod_Alt);
}

TEST(InputTest, GetModifiers_AllModifiers)
{
	const auto mods = Input::GetModifiers(
		static_cast<uint32_t>(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier));
	EXPECT_EQ(static_cast<int>(mods), Input::Mod_Shift | Input::Mod_Ctrl | Input::Mod_Alt);
	EXPECT_TRUE(mods & Input::Mod_Shift);
	EXPECT_TRUE(mods & Input::Mod_Ctrl);
	EXPECT_TRUE(mods & Input::Mod_Alt);
}

// ===========================================================================
// GetMouseButton — uint32_t → Input::MouseButton
// ===========================================================================

TEST(InputTest, GetMouseButton_LeftReturnsLeft)
{
	EXPECT_EQ(Input::GetMouseButton(static_cast<uint32_t>(Qt::LeftButton)), Input::MouseButton::Left);
}

TEST(InputTest, GetMouseButton_RightReturnsRight)
{
	EXPECT_EQ(Input::GetMouseButton(static_cast<uint32_t>(Qt::RightButton)), Input::MouseButton::Right);
}

TEST(InputTest, GetMouseButton_MiddleReturnsMiddle)
{
	EXPECT_EQ(Input::GetMouseButton(static_cast<uint32_t>(Qt::MiddleButton)), Input::MouseButton::Middle);
}

// ===========================================================================
// GetKey — uint32_t → Input::Key
// ===========================================================================

/// @test The three mode keys. A swap here rebinds a whole gesture silently.
TEST(InputTest, GetKey_ModeKeysMapToTheirModes)
{
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_G)), Input::Key::Key_G);
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_R)), Input::Key::Key_R);
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_S)), Input::Key::Key_S);
}

/// @test The three axis-constraint keys.
TEST(InputTest, GetKey_AxisKeysMapToTheirAxes)
{
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_X)), Input::Key::Key_X);
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_Y)), Input::Key::Key_Y);
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_Z)), Input::Key::Key_Z);
}

/**
 * @test W is a constraint key in its own right, not an alias for one of X/Y/Z.
 *
 * It names the screen as the constraint, which is what *drops* whichever axis is
 * latched. Folding it onto any of the three would turn "back to free" into "switch
 * to that axis" — a change no other test in this file could see.
 */
TEST(InputTest, GetKey_WIsItsOwnScreenSpaceKey)
{
	const Input::Key w = Input::GetKey(static_cast<uint32_t>(Qt::Key_W));
	EXPECT_EQ(w, Input::Key::Key_W);
	EXPECT_NE(w, Input::Key::Key_X);
	EXPECT_NE(w, Input::Key::Key_Y);
	EXPECT_NE(w, Input::Key::Key_Z);
}

/// @test Esc cancels; it must not collapse onto the confirm key.
TEST(InputTest, GetKey_EscapeIsDistinctFromReturn)
{
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_Escape)), Input::Key::Key_Escape);
	EXPECT_NE(Input::GetKey(static_cast<uint32_t>(Qt::Key_Escape)), Input::Key::Key_Return);
}

/**
 * @test Return and Enter are one key downstream.
 *
 * They are physically distinct (main block vs. numpad) and mean the same thing to
 * every modal operation, so they collapse at this boundary rather than forcing
 * every handler to test both.
 */
TEST(InputTest, GetKey_ReturnAndEnterBothConfirm)
{
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_Return)), Input::Key::Key_Return);
	EXPECT_EQ(Input::GetKey(static_cast<uint32_t>(Qt::Key_Enter)), Input::Key::Key_Return);
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
}

/// @test Key_Unknown is zero, which is what lets a handler test the key at all.
TEST(InputTest, GetKey_UnknownIsFalsy)
{
	EXPECT_EQ(static_cast<int>(Input::Key::Key_Unknown), 0);
}
