/**
 * @file test_i18n.cpp
 * @brief Unit tests for the I18n runtime translation manager.
 *
 * Covers the English built-in fallback, the zh_CN PO catalog lookup
 * (including msgctxt context disambiguation), unknown-language fallback, the
 * languageChanged() signal contract, and the supported-language registry.
 * English is restored in SetUp/TearDown so the singleton's state cannot leak
 * between tests.
 */

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>

#include <algorithm>

#include "ui/utils/I18n.h"

using namespace neurus;

namespace {

class I18nTest : public ::testing::Test
{
protected:
	void SetUp() override { I18n::instance().setLanguage("en"); }
	void TearDown() override { I18n::instance().setLanguage("en"); }
};

TEST_F(I18nTest, EnglishIsTheBuiltinFallback)
{
	I18n& i18n = I18n::instance();
	EXPECT_EQ(i18n.language(), QStringLiteral("en"));

	// No dictionary is loaded for English — keys ARE the strings.
	EXPECT_EQ(i18n.translate("&File"), QStringLiteral("&File"));
	EXPECT_EQ(i18n.translate("Anything at all"), QStringLiteral("Anything at all"));
}

TEST_F(I18nTest, ChineseCatalogTranslatesKnownKeys)
{
	I18n& i18n = I18n::instance();
	i18n.setLanguage("zh_CN");
	EXPECT_EQ(i18n.language(), QStringLiteral("zh_CN"));

	EXPECT_EQ(i18n.translate("&File"), QStringLiteral("文件(&F)"));
	EXPECT_EQ(i18n.translate("Algorithm"), QStringLiteral("算法"));

	// Untranslated keys fall back to English verbatim.
	EXPECT_EQ(i18n.translate("Some untranslated key"),
	          QStringLiteral("Some untranslated key"));
}

TEST_F(I18nTest, ContextsDisambiguateIdenticalStrings)
{
	I18n& i18n = I18n::instance();
	i18n.setLanguage("zh_CN");

	// "Viewport" is only translated in the "Dock" context (dock titles).
	EXPECT_EQ(i18n.translateCtx("Viewport", "Dock"), QStringLiteral("视口"));
	EXPECT_EQ(i18n.translate("Viewport"), QStringLiteral("Viewport"));

	// Tooltip vs. default context.
	EXPECT_EQ(i18n.translateCtx("Remove entry", "Tooltip"),
	          QStringLiteral("移除条目"));

	// Dialog context.
	EXPECT_EQ(i18n.translateCtx("Save Log", "Dialog"),
	          QStringLiteral("保存日志"));
	EXPECT_EQ(i18n.translate("Save Log"), QStringLiteral("Save Log"));
}

TEST_F(I18nTest, UnknownLanguageFallsBackToEnglish)
{
	I18n& i18n = I18n::instance();
	i18n.setLanguage("klingon");
	EXPECT_EQ(i18n.language(), QStringLiteral("klingon"));
	EXPECT_EQ(i18n.translate("&File"), QStringLiteral("&File"));
	EXPECT_EQ(i18n.translate("Viewport"), QStringLiteral("Viewport"));
}

TEST_F(I18nTest, LanguageChangedSignalContract)
{
	I18n& i18n = I18n::instance();
	QSignalSpy spy(&i18n, &I18n::languageChanged);

	i18n.setLanguage("zh_CN");
	EXPECT_EQ(spy.count(), 1);

	// Setting the same language again is a no-op — no signal.
	i18n.setLanguage("zh_CN");
	EXPECT_EQ(spy.count(), 1);

	i18n.setLanguage("en");
	EXPECT_EQ(spy.count(), 2);
}

TEST_F(I18nTest, SupportedLanguagesContainEnglishAndChinese)
{
	// Containment, not identity: adding res/i18n/<code>.po is meant to be a
	// one-file step (the CMake glob is CONFIGURE_DEPENDS and the registry sorts
	// by display name), so a count or a positional index would turn that
	// one-file step into a two-file step for no reason.
	const auto langs = I18n::supportedLanguages();

	const auto has = [&langs](const QString& code) {
		return std::any_of(langs.cbegin(), langs.cend(), [&code](const auto& l) {
			return l.code == code && !l.displayName.isEmpty();
		});
	};

	EXPECT_TRUE(has(QStringLiteral("en")));      // built-in, never a .po file
	EXPECT_TRUE(has(QStringLiteral("zh_CN")));   // res/i18n/zh_CN.po
}

TEST_F(I18nTest, SystemLanguageIsOneOfTheSupportedLanguages)
{
	// systemLanguage() reads the host locale, so the concrete answer depends on
	// the machine — what must hold is that it is always a language we can
	// actually switch to, otherwise "auto" would resolve to a dead code.
	const QString lang = I18n::systemLanguage();
	const auto langs = I18n::supportedLanguages();

	EXPECT_TRUE(std::any_of(langs.cbegin(), langs.cend(),
	                        [&lang](const auto& l) { return l.code == lang; }))
		<< "systemLanguage() returned '" << lang.toStdString()
		<< "', which is not in supportedLanguages()";
}

TEST_F(I18nTest, AutoLanguageResolvesToSystemLanguage)
{
	// Safety net: "auto" (used by Preferences before the Application resolves
	// it) is treated as "follow the system UI language".
	I18n& i18n = I18n::instance();
	i18n.setLanguage(QStringLiteral("auto"));
	EXPECT_NE(i18n.language(), QStringLiteral("auto"));
	EXPECT_TRUE(i18n.language() == QStringLiteral("en")
	            || i18n.language() == QStringLiteral("zh_CN"));
}

} // namespace
