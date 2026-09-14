/**
 * @file test_dock_identity.cpp
 * @brief Regression tests: dock serialization identity is language-independent.
 *
 * ads::CDockWidget derives its objectName from the ctor title, and ADS saves and
 * restores layouts keyed by objectName. Once dock titles became translatable, a
 * layout saved in one language could no longer be restored in another — every
 * lookup missed and the window came up with no panels.
 *
 * These tests pin the fix from both ends:
 * - PanelIdFor() is a stable ASCII identity, unaffected by the active language.
 * - A layout exported under one language restores completely under another, and
 *   an unusable blob degrades to a populated default layout rather than a blank
 *   window.
 */

#include <gtest/gtest.h>

#include <QSet>
#include <QString>

#include <DockManager.h>
#include <DockWidget.h>

#include "ui/UIManager.h"
#include "ui/panels/UIPanel.h"
#include "ui/utils/I18n.h"

using namespace neurus;

namespace {

/** @brief Every PanelType except the Count sentinel. */
std::vector<PanelType> AllPanelTypes()
{
	std::vector<PanelType> types;
	for (uint8_t i = 0; i < static_cast<uint8_t>(PanelType::Count); ++i)
		types.push_back(static_cast<PanelType>(i));
	return types;
}

/** @brief Constructs a main window with default (English-seeded) preferences. */
std::unique_ptr<UIManager> MakeWindow()
{
	return std::make_unique<UIManager>(QStringLiteral("en"), 60, QString());
}

/** @brief Returns the docks the restored layout failed to claim. */
QStringList OrphanedDocks(const UIManager& win)
{
	QStringList orphans;
	for (PanelType type : AllPanelTypes())
	{
		auto* dock = win.GetDock(type);
		if (dock && !dock->dockAreaWidget())
			orphans << dock->objectName();
	}
	return orphans;
}

class DockIdentityTest : public ::testing::Test
{
protected:
	void SetUp() override { I18n::instance().setLanguage("en"); }
	void TearDown() override { I18n::instance().setLanguage("en"); }
};

TEST_F(DockIdentityTest, PanelIdsAreNonEmptyUniqueAscii)
{
	QSet<QString> seen;
	for (PanelType type : AllPanelTypes())
	{
		const QString id = QString::fromLatin1(UIPanel::PanelIdFor(type));
		EXPECT_FALSE(id.isEmpty()) << "type " << int(type);
		EXPECT_NE(id, QStringLiteral("dock.unknown")) << "type " << int(type);

		// The id lands in a project file; keep it plain ASCII so it survives any
		// encoding the archive happens to use.
		for (QChar c : id)
			EXPECT_TRUE(c.unicode() < 128) << "non-ASCII in id " << id.toStdString();

		EXPECT_FALSE(seen.contains(id)) << "duplicate id " << id.toStdString();
		seen.insert(id);
	}
	EXPECT_EQ(seen.size(), static_cast<int>(PanelType::Count));
}

TEST_F(DockIdentityTest, PanelIdsDoNotFollowTheActiveLanguage)
{
	std::vector<QString> english;
	for (PanelType type : AllPanelTypes())
		english.push_back(QString::fromLatin1(UIPanel::PanelIdFor(type)));

	I18n::instance().setLanguage("zh_CN");

	for (size_t i = 0; i < english.size(); ++i)
	{
		const auto type = static_cast<PanelType>(i);
		EXPECT_EQ(QString::fromLatin1(UIPanel::PanelIdFor(type)), english[i]);
	}
}

TEST_F(DockIdentityTest, DockObjectNamesAreIdsNotTitles)
{
	auto win = MakeWindow();
	I18n::instance().setLanguage("zh_CN");

	for (PanelType type : AllPanelTypes())
	{
		auto* dock = win->GetDock(type);
		ASSERT_NE(dock, nullptr) << "type " << int(type);
		EXPECT_EQ(dock->objectName(), QString::fromLatin1(UIPanel::PanelIdFor(type)));
		// The title is free to follow the language; the key is not.
		EXPECT_NE(dock->objectName(), dock->windowTitle());
	}
}

// The acceptance test for the whole change: save in one language, restore in the
// other. A live language switch alone would NOT catch a regression here —
// setWindowTitle() leaves objectName alone, so breakage only surfaces on the
// next launch, which is exactly what this exercises.
TEST_F(DockIdentityTest, LayoutRoundTripsAcrossLanguages)
{
	std::string blob;
	{
		I18n::instance().setLanguage("zh_CN");
		auto saver = MakeWindow();
		blob = saver->ExportLayout();
	}
	ASSERT_FALSE(blob.empty());

	I18n::instance().setLanguage("en");
	auto loader = MakeWindow();
	loader->ApplyLayout(blob);

	EXPECT_TRUE(OrphanedDocks(*loader).isEmpty())
		<< "docks unclaimed by a zh_CN-saved layout: "
		<< OrphanedDocks(*loader).join(QStringLiteral(", ")).toStdString();
}

TEST_F(DockIdentityTest, LayoutRoundTripsInTheOppositeDirection)
{
	std::string blob;
	{
		I18n::instance().setLanguage("en");
		auto saver = MakeWindow();
		blob = saver->ExportLayout();
	}
	ASSERT_FALSE(blob.empty());

	I18n::instance().setLanguage("zh_CN");
	auto loader = MakeWindow();
	loader->ApplyLayout(blob);

	EXPECT_TRUE(OrphanedDocks(*loader).isEmpty())
		<< "docks unclaimed by an en-saved layout: "
		<< OrphanedDocks(*loader).join(QStringLiteral(", ")).toStdString();
}

// A blob keyed by dock names from before the fix (or from any foreign source)
// must not leave the user with an empty window.
//
// The fixture is the real dock state that was committed into
// res/shadow.neurus.json: qCompress'd ADS XML whose eight Widget Name entries
// are Chinese dock titles (大纲, 视口, 属性面板, …). ADS parses it happily and
// returns true, silently skipping every name it does not recognise, so
// restoreState()'s return value cannot detect this — only the repair pass can.
TEST_F(DockIdentityTest, ForeignLayoutFallsBackToPopulatedDocks)
{
	auto win = MakeWindow();

	static constexpr const char* kLegacyChineseState =
		"AAADi3jajZNRS8JQGIbv+xWH3dvmFurFnIjhZRFqXS93kNHcYjtKRRdCJEVERpAEqVDJugryqiTz"
		"1+yc+S86MxlzOe1i7Hzf+53ne1/YxMxRVQN1aFqqoaeZ+DrHAKiXDUXVK2mmVMzHUkxGEndQVqnL"
		"ehkqm0b5gGqFYwvBKtj1LzKgZEHTrykmZ+hIVnXa8WRJ9GuQ1wwZTRdwtF841FSEaHvbVCEdQVNA"
		"zAPUdJRm+MiZU39G+Ccna0IZFOX9qSWQq5km9CTnxSbDAdX3VKUCEdiSq9DvgpxmWFDxzLKSyHqI"
		"aNB7FzfsSecJd8Z/cEFtIbSgnkBLEgQOeI/I/tb0PUsWuda1m87Nc3jhrLvKPx8A4Y8B7t1Nzq/J"
		"6C2MI52GezlwHl7J1737fUsPIfTc9BxpWVouBVLxOBC45KrEQtBow3bPRs5FE3dbYaO43XfG7WXm"
		"yPCTtJq41yePV6tyBBctyZFIJAHPbyzMwPqfPj1H/ErS2g8RZFP5";

	win->ApplyLayout(std::string("\n") + kLegacyChineseState);

	EXPECT_TRUE(OrphanedDocks(*win).isEmpty())
		<< "blank window after a foreign layout; unclaimed: "
		<< OrphanedDocks(*win).join(QStringLiteral(", ")).toStdString();
}

TEST_F(DockIdentityTest, MalformedLayoutLeavesTheDefaultLayoutIntact)
{
	auto win = MakeWindow();

	win->ApplyLayout("not-base64-and-has-no-separator");
	EXPECT_TRUE(OrphanedDocks(*win).isEmpty());

	win->ApplyLayout(std::string("\n") + "bm90IHhtbA==");  // base64("not xml")
	EXPECT_TRUE(OrphanedDocks(*win).isEmpty());
}

} // namespace
