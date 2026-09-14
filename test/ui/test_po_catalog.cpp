/**
 * @file test_po_catalog.cpp
 * @brief Unit tests for the gettext .po parser behind I18n.
 *
 * The two regressions these pin are both "the file is valid gettext but not
 * shaped the way our own generator shapes it":
 *
 * - Blank lines between entries are conventional, not required. Treating a
 *   blank line as the only entry terminator made each entry inherit the
 *   previous one's msgstr, and since every catalog opens with the metadata
 *   header (msgid ""), the first real key resolved to the header text.
 * - Plural entries use msgid_plural / msgstr[N]. "msgid" and "msgstr" are
 *   prefixes of those, so a startsWith() test appended the plural payload onto
 *   the singular fields.
 *
 * Both shapes are what hand-editing and Poedit produce, which is the whole
 * reason the .po format was chosen over a bespoke one.
 */

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include "ui/utils/POCatalog.h"

using namespace neurus;

namespace {

/** @brief Looks a key up the way I18n::translateCtx does. */
QString Lookup(const po::Catalog& cat, const char* msgid, const char* context = "")
{
	const auto it = cat.constFind({ QString::fromUtf8(context),
	                                QString::fromUtf8(msgid) });
	return it != cat.constEnd() ? it.value() : QString();
}

/** @brief The canonical header entry every generated catalog starts with. */
constexpr const char* kHeader =
	"msgid \"\"\n"
	"msgstr \"\"\n"
	"\"Project-Id-Version: Neurus\\n\"\n"
	"\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
	"\"X-Language-Name: 简体中文\\n\"\n";

TEST(POCatalogTest, ParsesTranslatedEntriesAndSkipsTheHeader)
{
	const QByteArray data = QByteArray(kHeader) +
		"\n"
		"msgid \"Viewport\"\n"
		"msgstr \"视口\"\n"
		"\n"
		"msgctxt \"Dock\"\n"
		"msgid \"Log\"\n"
		"msgstr \"日志\"\n";

	po::Catalog cat;
	const po::Stats stats = po::Parse(data, cat);

	EXPECT_EQ(stats.total, 2) << "the header entry must not be counted";
	EXPECT_EQ(stats.translated, 2);
	EXPECT_EQ(Lookup(cat, "Viewport"), QStringLiteral("视口"));
	EXPECT_EQ(Lookup(cat, "Log", "Dock"), QStringLiteral("日志"));
	// A contexted entry must not answer a default-context lookup.
	EXPECT_TRUE(Lookup(cat, "Log").isEmpty());
}

TEST(POCatalogTest, UntranslatedEntriesAreCountedButNotStored)
{
	const QByteArray data =
		"msgid \"Translated\"\n"
		"msgstr \"有\"\n"
		"\n"
		"msgid \"Untranslated\"\n"
		"msgstr \"\"\n";

	po::Catalog cat;
	const po::Stats stats = po::Parse(data, cat);

	EXPECT_EQ(stats.total, 2);
	EXPECT_EQ(stats.translated, 1);
	EXPECT_EQ(cat.size(), 1) << "an empty msgstr must fall back to the English key";
	EXPECT_TRUE(Lookup(cat, "Untranslated").isEmpty());
}

// The regression: no blank line after the header. Every key that followed used
// to come back as the header's field block.
TEST(POCatalogTest, HeaderDoesNotBleedIntoTheFirstEntry)
{
	const QByteArray data = QByteArray(kHeader) +
		"msgid \"Viewport\"\n"
		"msgstr \"视口\"\n";

	po::Catalog cat;
	const po::Stats stats = po::Parse(data, cat);

	EXPECT_EQ(stats.total, 1);
	EXPECT_EQ(Lookup(cat, "Viewport"), QStringLiteral("视口"));
}

// Same defect, between two ordinary entries: without the blank line the first
// entry was dropped outright (its msgid was overwritten before any flush).
TEST(POCatalogTest, EntriesWithoutBlankSeparatorsAreAllKept)
{
	const QByteArray data =
		"msgid \"One\"\n"
		"msgstr \"一\"\n"
		"msgid \"Two\"\n"
		"msgstr \"二\"\n"
		"msgctxt \"Dock\"\n"
		"msgid \"Three\"\n"
		"msgstr \"三\"\n"
		"msgid \"Four\"\n"
		"msgstr \"四\"\n";

	po::Catalog cat;
	const po::Stats stats = po::Parse(data, cat);

	EXPECT_EQ(stats.total, 4);
	EXPECT_EQ(Lookup(cat, "One"), QStringLiteral("一"));
	EXPECT_EQ(Lookup(cat, "Two"), QStringLiteral("二"));
	EXPECT_EQ(Lookup(cat, "Three", "Dock"), QStringLiteral("三"));
	// The context must not leak past the entry that declared it.
	EXPECT_EQ(Lookup(cat, "Four"), QStringLiteral("四"));
}

TEST(POCatalogTest, PluralFormsDoNotCorruptTheSingular)
{
	const QByteArray data =
		"msgid \"%1 entry\"\n"
		"msgid_plural \"%1 entries\"\n"
		"msgstr[0] \"%1 条\"\n"
		"msgstr[1] \"%1 条目\"\n"
		"\n"
		"msgid \"After\"\n"
		"msgstr \"之后\"\n";

	po::Catalog cat;
	const po::Stats stats = po::Parse(data, cat);

	EXPECT_EQ(stats.total, 2);
	// msgid_plural must be dropped, not appended to the msgid...
	EXPECT_EQ(Lookup(cat, "%1 entry"), QStringLiteral("%1 条"))
		<< "msgstr[0] is the singular translation";
	// ...and msgstr[1] must not be appended to msgstr[0].
	EXPECT_EQ(Lookup(cat, "After"), QStringLiteral("之后"));
}

// Continuation lines belong to the field that opened them; an ignored field
// must swallow its own continuations rather than leak them into the next one.
TEST(POCatalogTest, ContinuationLinesAccumulateOnTheRightField)
{
	const QByteArray data =
		"msgid \"\"\n"
		"\"Long \"\n"
		"\"key\"\n"
		"msgstr \"\"\n"
		"\"Long \"\n"
		"\"value\"\n"
		"\n"
		"msgid \"Plural\"\n"
		"msgid_plural \"\"\n"
		"\"ignored \"\n"
		"\"payload\"\n"
		"msgstr[0] \"ok\"\n";

	po::Catalog cat;
	po::Parse(data, cat);

	EXPECT_EQ(Lookup(cat, "Long key"), QStringLiteral("Long value"));
	EXPECT_EQ(Lookup(cat, "Plural"), QStringLiteral("ok"));
}

TEST(POCatalogTest, ObsoleteEntriesAndCommentsAreIgnored)
{
	const QByteArray data =
		"# translator comment\n"
		"msgid \"Live\"\n"
		"msgstr \"在\"\n"
		"\n"
		"#~ msgid \"Removed\"\n"
		"#~ msgstr \"已移除\"\n";

	po::Catalog cat;
	const po::Stats stats = po::Parse(data, cat);

	EXPECT_EQ(stats.total, 1);
	EXPECT_TRUE(Lookup(cat, "Removed").isEmpty())
		<< "an obsolete entry is not an active translation";
}

TEST(POCatalogTest, EscapesAreDecoded)
{
	const QByteArray data =
		"msgid \"A \\\"quoted\\\" key\"\n"
		"msgstr \"line1\\nline2\\ttabbed\"\n";

	po::Catalog cat;
	po::Parse(data, cat);

	EXPECT_EQ(Lookup(cat, "A \"quoted\" key"),
	          QStringLiteral("line1\nline2\ttabbed"));
}

TEST(POCatalogTest, HeaderFieldReadsMetadataAndStopsAtTheFirstEntry)
{
	const QByteArray data = QByteArray(kHeader) +
		"\n"
		"msgid \"Viewport\"\n"
		"msgstr \"视口\"\n"
		"\"X-Language-Name: wrong\\n\"\n";

	EXPECT_EQ(po::HeaderField(data, QLatin1String("X-Language-Name")),
	          QStringLiteral("简体中文"));
	EXPECT_TRUE(po::HeaderField(data, QLatin1String("X-Absent")).isEmpty());
}

TEST(POCatalogTest, EmptyInputYieldsNothing)
{
	po::Catalog cat;
	const po::Stats stats = po::Parse(QByteArray(), cat);

	EXPECT_EQ(stats.total, 0);
	EXPECT_EQ(stats.translated, 0);
	EXPECT_TRUE(cat.isEmpty());
}

} // namespace
