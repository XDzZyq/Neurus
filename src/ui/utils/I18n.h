/**
 * @file I18n.h
 * @brief Lightweight runtime UI translation manager (gettext-style PO).
 *
 * Translation keys are the English display strings themselves (the built-in
 * fallback). Per-language catalogs are **GNU gettext .po files**
 * (res/i18n/<code>.po), embedded as Qt resources (:/i18n/<code>.po) and
 * parsed on demand — the same format Blender uses, so translators can work
 * with Poedit / Weblate and the extraction pipeline in
 * scripts/extract_i18n.py keeps the catalogs in sync with the code.
 *
 * Contexts (msgctxt) disambiguate identical English strings:
 *   translate(key)              -> default context
 *   translateCtx(key, context)  -> contexted lookup
 * A missing catalog, context or msgid falls back to the English key itself.
 *
 * setLanguage() swaps the active catalog and emits languageChanged() so every
 * widget can retranslate immediately — no application restart required.
 * UIManager connects to languageChanged() and re-applies menu texts, dock
 * titles and every panel's Retranslate() hook.
 */

#pragma once

/**
 * @brief No-op marker for strings that are translated at runtime through I18n
 *        but do not appear as literal translate() arguments at the call
 *        site (e.g. keys forwarded to menu-builder helpers). Exists so
 *        scripts/extract_i18n.py can discover the msgid — the gettext N_
 *        convention.
 */
#define N_(text) text

#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

namespace neurus {

class I18n : public QObject
{
	Q_OBJECT

public:
	/** @brief Returns the process-wide translation manager. */
	static I18n& instance();

	/** @brief Active language code ("en", "zh_CN", ...). */
	QString language() const { return m_language; }

	/**
	 * @brief Switches the active language and emits languageChanged().
	 * @param code Language code ("en", "zh_CN"). Empty, "auto" or unknown
	 *             codes fall back to the built-in English catalog (keys).
	 */
	void setLanguage(const QString& code);

	/**
	 * @brief Translates @p key in the default context.
	 * @param key English source string (also the catalog msgid).
	 * @return The translated string, or @p key itself when untranslated.
	 */
	QString translate(const char* key) const;

	/**
	 * @brief Translates @p key within @p context (gettext msgctxt).
	 * @param key     English source string (msgid).
	 * @param context Disambiguating context, e.g. "Dock", "Tooltip", "Dialog".
	 * @return The translated string, or @p key itself when untranslated.
	 */
	QString translateCtx(const char* key, const char* context) const;

	/** @brief One supported language: code + native display name. */
	struct LanguageInfo
	{
		QString code;        ///< e.g. "en", "zh_CN"
		QString displayName; ///< native name, e.g. "English", "简体中文"
	};

	/**
	 * @brief All languages shipped with the app.
	 *
	 * English first (it has no catalog — the msgids are the English strings),
	 * then one entry per embedded res/i18n/<code>.po, sorted by code. The
	 * display name comes from the catalog's X-Language-Name header field,
	 * falling back to QLocale's native language name.
	 */
	static QList<LanguageInfo> supportedLanguages();

	/**
	 * @brief Detects the system UI language.
	 * @return "zh_CN" on Simplified-Chinese systems, "en" otherwise.
	 */
	static QString systemLanguage();

signals:
	/** @brief Emitted after the active catalog was swapped. */
	void languageChanged();

private:
	I18n() = default;
	~I18n() override = default;
	I18n(const I18n&) = delete;
	I18n& operator=(const I18n&) = delete;

	/** @brief Loads (and replaces) the catalog for @p code from resources. */
	void loadCatalog(const QString& code);

	QString m_language = QStringLiteral("en");

	/** @brief (context, msgid) -> translation; empty context = default. */
	QHash<QPair<QString, QString>, QString> m_dict;
};

} // namespace neurus
