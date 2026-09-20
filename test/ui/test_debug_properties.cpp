/**
 * @file test_debug_properties.cpp
 * @brief Widget tests for the DebugProperties preset (issue #22).
 *
 * The preset is the only one in src/ui/presets/ that edits a *list* rather than
 * a set of scalars, so the parts worth pinning are the ones a scalar preset has
 * no equivalent of:
 * - the dirty-checked setters stay silent (a per-frame Refresh() must not fight
 *   the widget the user is typing in);
 * - a cell edit, an Add and a Remove all emit the whole list;
 * - the row cap turns the table read-only instead of silently truncating.
 */

#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>

#include <glm/glm.hpp>

#include <vector>

#include "scene/ObjectID.h"
#include "ui/items/ColorButton.h"
#include "ui/items/ScalarSlider.h"
#include "ui/presets/DebugProperties.h"

// QSignalSpy stores each argument as a QVariant, so the two non-Qt payload types
// the preset emits need a metatype before a spy can read them back.
Q_DECLARE_METATYPE(glm::vec4)
Q_DECLARE_METATYPE(std::vector<glm::vec3>)

using namespace neurus;

namespace {

int GoType(ObjectID::GOType t) { return static_cast<int>(t); }

/** @brief The preset's position table, found by type (it owns exactly one). */
QTableWidget* Table(DebugProperties& props)
{
	return props.findChild<QTableWidget*>();
}

/**
 * @brief The Add / Remove buttons, in construction order.
 *
 * Found by type rather than by label: the labels are translated, and another
 * test in this binary may have left a non-English language active. ColorButton
 * is itself a QPushButton, so it is filtered out explicitly.
 */
std::vector<QPushButton*> ListButtons(DebugProperties& props)
{
	std::vector<QPushButton*> out;
	for (QPushButton* b : props.findChildren<QPushButton*>())
	{
		if (!qobject_cast<ColorButton*>(b))
			out.push_back(b);
	}
	return out;
}

std::vector<glm::vec3> MakePositions(int count)
{
	std::vector<glm::vec3> out;
	out.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i)
	{
		const float f = static_cast<float>(i);
		out.emplace_back(f, f + 0.5f, f + 0.25f);
	}
	return out;
}

} // namespace

class DebugPropertiesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		qRegisterMetaType<glm::vec4>();
		qRegisterMetaType<std::vector<glm::vec3>>();

		m_props = new DebugProperties();
		m_props->setObjectId(7);
		m_props->setDebugType(GoType(ObjectID::GOType::GO_DL));
	}

	void TearDown() override { delete m_props; }

	DebugProperties* m_props = nullptr;
};
// PLACEHOLDER_TESTS

// --- Setters are silent ------------------------------------------------------

TEST_F(DebugPropertiesTest, ProgrammaticSettersEmitNothing)
{
	QSignalSpy color(m_props, &DebugProperties::colorChanged);
	QSignalSpy opacity(m_props, &DebugProperties::opacityChanged);
	QSignalSpy xray(m_props, &DebugProperties::xrayChanged);
	QSignalSpy width(m_props, &DebugProperties::lineWidthChanged);
	QSignalSpy stipple(m_props, &DebugProperties::stippleChanged);
	QSignalSpy smooth(m_props, &DebugProperties::smoothChanged);
	QSignalSpy pointType(m_props, &DebugProperties::pointTypeChanged);
	QSignalSpy scale(m_props, &DebugProperties::pointScaleChanged);
	QSignalSpy projection(m_props, &DebugProperties::projectionModeChanged);
	QSignalSpy positions(m_props, &DebugProperties::positionsChanged);

	m_props->setColor(glm::vec4(0.2f, 0.4f, 0.6f, 0.8f));
	m_props->setOpacity(0.5f);
	m_props->setXRay(true);
	m_props->setLineWidth(3.0f);
	m_props->setStipple(true);
	m_props->setSmooth(true);
	m_props->setPointType(2);
	m_props->setPointScale(16.0f);
	m_props->setProjectionMode(1);
	m_props->setPositions(MakePositions(4));

	EXPECT_EQ(color.count(), 0);
	EXPECT_EQ(opacity.count(), 0);
	EXPECT_EQ(xray.count(), 0);
	EXPECT_EQ(width.count(), 0);
	EXPECT_EQ(stipple.count(), 0);
	EXPECT_EQ(smooth.count(), 0);
	EXPECT_EQ(pointType.count(), 0);
	EXPECT_EQ(scale.count(), 0);
	EXPECT_EQ(projection.count(), 0);
	EXPECT_EQ(positions.count(), 0);
}

TEST_F(DebugPropertiesTest, SetPositionsFillsTheTable)
{
	m_props->setPositions(MakePositions(3));

	QTableWidget* table = Table(*m_props);
	ASSERT_NE(table, nullptr);
	EXPECT_EQ(table->rowCount(), 3);
	EXPECT_EQ(table->columnCount(), 3);
	EXPECT_DOUBLE_EQ(table->item(1, 0)->text().toDouble(), 1.0);
	EXPECT_DOUBLE_EQ(table->item(1, 1)->text().toDouble(), 1.5);
	EXPECT_DOUBLE_EQ(table->item(1, 2)->text().toDouble(), 1.25);
}
// PLACEHOLDER_TESTS2

// --- List editing emits the whole list --------------------------------------

TEST_F(DebugPropertiesTest, CellEditEmitsTheWholeList)
{
	m_props->setPositions(MakePositions(2));
	QSignalSpy spy(m_props, &DebugProperties::positionsChanged);

	Table(*m_props)->item(0, 0)->setText("9.5");

	ASSERT_EQ(spy.count(), 1);
	EXPECT_EQ(spy.at(0).at(0).toInt(), 7);
	const auto emitted = spy.at(0).at(1).value<std::vector<glm::vec3>>();
	ASSERT_EQ(emitted.size(), 2u);
	EXPECT_FLOAT_EQ(emitted[0].x, 9.5f);
	EXPECT_FLOAT_EQ(emitted[1].x, 1.0f); // the untouched row still travels
}

TEST_F(DebugPropertiesTest, UnparseableCellKeepsThePreviousCoordinate)
{
	m_props->setPositions(MakePositions(1));
	QSignalSpy spy(m_props, &DebugProperties::positionsChanged);

	// A typo mid-edit must not teleport the vertex to the origin: the cell reads
	// back as the coordinate it replaced, so the list is unchanged and silent.
	Table(*m_props)->item(0, 1)->setText("not a number");

	EXPECT_EQ(spy.count(), 0);
}

TEST_F(DebugPropertiesTest, AddEmitsAListOneLonger)
{
	m_props->setPositions(MakePositions(2));
	QSignalSpy spy(m_props, &DebugProperties::positionsChanged);

	auto buttons = ListButtons(*m_props);
	ASSERT_EQ(buttons.size(), 2u);
	buttons[0]->click(); // Add

	ASSERT_EQ(spy.count(), 1);
	const auto emitted = spy.at(0).at(1).value<std::vector<glm::vec3>>();
	ASSERT_EQ(emitted.size(), 3u);
	// Seeded from the last entry, so appending continues the line instead of
	// dropping a degenerate segment at the origin.
	EXPECT_EQ(emitted[2], emitted[1]);
}

TEST_F(DebugPropertiesTest, RemoveWithNoSelectionDropsTheLastEntry)
{
	m_props->setPositions(MakePositions(3));
	QSignalSpy spy(m_props, &DebugProperties::positionsChanged);

	auto buttons = ListButtons(*m_props);
	ASSERT_EQ(buttons.size(), 2u);
	buttons[1]->click(); // Remove

	ASSERT_EQ(spy.count(), 1);
	const auto emitted = spy.at(0).at(1).value<std::vector<glm::vec3>>();
	ASSERT_EQ(emitted.size(), 2u);
	EXPECT_FLOAT_EQ(emitted[1].x, 1.0f);
}

TEST_F(DebugPropertiesTest, UnboundPanelEmitsNothingOnACellEdit)
{
	m_props->setPositions(MakePositions(2));
	m_props->setObjectId(-1); // nothing selected
	QSignalSpy spy(m_props, &DebugProperties::positionsChanged);

	Table(*m_props)->item(0, 0)->setText("4.0");

	EXPECT_EQ(spy.count(), 0);
}
// PLACEHOLDER_TESTS3

// --- Row cap ----------------------------------------------------------------

TEST_F(DebugPropertiesTest, OversizedListGoesReadOnlyInsteadOfTruncating)
{
	m_props->setPositions(MakePositions(600));

	QTableWidget* table = Table(*m_props);
	ASSERT_NE(table, nullptr);
	EXPECT_EQ(table->rowCount(), 512);
	EXPECT_TRUE(table->editTriggers() == QAbstractItemView::NoEditTriggers);

	// Add / Remove would write back only the visible rows and silently drop the
	// hidden tail, so they are disabled rather than destructive.
	for (QPushButton* b : ListButtons(*m_props))
		EXPECT_FALSE(b->isEnabled());
}

// --- Type switching ---------------------------------------------------------

TEST_F(DebugPropertiesTest, DebugMeshHidesThePositionTable)
{
	m_props->setDebugType(GoType(ObjectID::GOType::GO_DP));
	EXPECT_TRUE(Table(*m_props)->isVisibleTo(m_props));

	// A DebugMesh's geometry is a pooled MeshData; there is no editable list.
	m_props->setDebugType(GoType(ObjectID::GOType::GO_DM));
	EXPECT_FALSE(Table(*m_props)->isVisibleTo(m_props));
}

TEST_F(DebugPropertiesTest, SetObjectIdInvalidatesTheCaches)
{
	// The opacity row stands in for every dirty-checked scalar.
	auto sliders = m_props->findChildren<ScalarSlider*>();
	ASSERT_FALSE(sliders.isEmpty());
	ScalarSlider* opacity = sliders.first();

	m_props->setOpacity(0.5f);
	ASSERT_DOUBLE_EQ(opacity->value(), 0.5);

	// A user edit leaves the cache at the last pushed value, so the Refresh()
	// that re-pushes it must not yank the control out from under the user.
	QSignalSpy spy(m_props, &DebugProperties::opacityChanged);
	opacity->spinBox()->setValue(0.8);
	ASSERT_EQ(spy.count(), 1);
	m_props->setOpacity(0.5f);
	EXPECT_DOUBLE_EQ(opacity->value(), 0.8);

	// Re-binding to another object drops that cache, so the new object's value
	// writes through even when it equals what was last pushed.
	m_props->setObjectId(8);
	m_props->setOpacity(0.5f);
	EXPECT_DOUBLE_EQ(opacity->value(), 0.5);
	EXPECT_EQ(spy.count(), 1); // nothing above counted as a user gesture
}
