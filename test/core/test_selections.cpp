/**
 * @file test_selections.cpp
 * @brief Unit tests for Selections<T> with int (value-semantics logic tests).
 *
 * Tests cover:
 * - Default construction: empty, no active
 * - Single select (increment=false): replaces prior selection
 * - Multi-select (increment=true): adds to set
 * - Deselect: removes from set
 * - Active object: tracks the last-selected value
 * - ClearSelection: empties everything
 * - IsSelected: O(1) membership query
 * - GetSelectedList: ordered iteration
 * - Serialization roundtrip via cereal JSONArchive
 */

#include <gtest/gtest.h>

#include <sstream>

#include <cereal/archives/json.hpp>

#include "core/Selections.hpp"

using namespace neurus;

// ============================================================================
// Select — single (increment=false)
// ============================================================================

TEST(SelectionsTest, Select_Single_ReplacesPrior)
{
	Selections<int> sel;
	EXPECT_EQ(sel.GetSelectionCount(), 0u);
	EXPECT_EQ(sel.GetActiveObject(), 0);
	sel.Select(10, false);
	sel.Select(20, false);
	sel.Select(20, false);

	EXPECT_EQ(sel.GetSelectionCount(), 1u);
	EXPECT_TRUE(sel.IsSelected(20));
	EXPECT_FALSE(sel.IsSelected(10));
	EXPECT_FALSE(sel.IsSelected(0));
	EXPECT_EQ(sel.GetSelectedObjects(), 20);
	EXPECT_EQ(sel.GetActiveObject(), 20);
}

// ============================================================================
// Select — multi (increment=true)
// ============================================================================

TEST(SelectionsTest, Select_Increment_MovesToActive)
{
	Selections<int> sel;
	sel.Select(1, true);
	sel.Select(2, true);
	sel.Select(3, true);

	EXPECT_EQ(sel.GetSelectedList(), (std::vector<int>{1, 2, 3}));
	// Re-select 1 — it becomes active (moved to end)
	sel.Select(1, true);

	EXPECT_EQ(sel.GetSelectionCount(), 3u);
	EXPECT_EQ(sel.GetActiveObject(), 1);

	// Order: 3, 2, 1 (std::swap exchanges positions, 1 ends at back)
	const auto& list = sel.GetSelectedList();
	ASSERT_EQ(list.size(), 3u);
	EXPECT_EQ(list[0], 3);
	EXPECT_EQ(list[1], 2);
	EXPECT_EQ(list[2], 1);
	sel.Deselect(1, false);

	EXPECT_EQ(sel.GetActiveObject(), 2);
	EXPECT_EQ(sel.GetSelectionCount(), 2u);
	EXPECT_TRUE(sel.IsSelected(2));
	EXPECT_TRUE(sel.IsSelected(3));
}

// ============================================================================
// Deselect
// ============================================================================

TEST(SelectionsTest, Deselect_RemovesFromSelection)
{
	Selections<int> sel;
	sel.Select(42, false);
	sel.Deselect(99, false);
	EXPECT_TRUE(sel.IsSelected(42));
	sel.Deselect(42, false);

	EXPECT_TRUE(sel.GetSelectedList().empty());
	EXPECT_FALSE(sel.IsSelected(42));
	EXPECT_EQ(sel.GetActiveObject(), 0);
}

// ============================================================================
// ClearSelection
// ============================================================================

TEST(SelectionsTest, ClearSelection_RemovesAll)
{
	Selections<int> sel;
	sel.Select(1, true);
	sel.Select(2, true);
	sel.Select(3, true);

	sel.ClearSelection();
	sel.ClearSelection();

	EXPECT_TRUE(sel.GetSelectedList().empty());
	EXPECT_EQ(sel.GetActiveObject(), 0);
	EXPECT_FALSE(sel.IsSelected(1));
}

// ============================================================================
// Serialization roundtrip
// ============================================================================

TEST(SelectionsTest, SerializationRoundtrip)
{
	Selections<int> sel;
	sel.Select(100, true);
	sel.Select(200, true);
	sel.Select(300, true);

	// Serialize to JSON string
	std::ostringstream oss;
	{
		cereal::JSONOutputArchive ar(oss);
		ar(cereal::make_nvp("selection", sel));
	}

	// Deserialize into a fresh instance
	Selections<int> restored;
	{
		std::istringstream iss(oss.str());
		cereal::JSONInputArchive ar(iss);
		ar(cereal::make_nvp("selection", restored));
	}

	// Verify deserialized state
	EXPECT_EQ(restored.GetSelectionCount(), 3u);
	EXPECT_TRUE(restored.IsSelected(100));
	EXPECT_TRUE(restored.IsSelected(200));
	EXPECT_TRUE(restored.IsSelected(300));

	const auto& list = restored.GetSelectedList();
	ASSERT_EQ(list.size(), 3u);
	EXPECT_EQ(list[0], 100);
	EXPECT_EQ(list[1], 200);
	EXPECT_EQ(list[2], 300);

	// Active is rebuilt as the last element after deserialization
	EXPECT_EQ(restored.GetActiveObject(), 300);
}
