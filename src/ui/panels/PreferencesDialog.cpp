#include "panels/PreferencesDialog.h"

#include "ui/utils/I18n.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace neurus {

namespace {

/// The "follow the system UI language" sentinel, stored verbatim in
/// ~/.neurus/preferences.json and resolved by I18n::setLanguage().
constexpr QLatin1String kAutoLanguage("auto");

/// Row of the language combo that carries kAutoLanguage.
constexpr int kAutoLanguageIndex = 0;

} // namespace

PreferencesDialog::PreferencesDialog(const QString& language, int targetFps,
                                     const QString& preferencesPath,
                                     QWidget* parent)
	: QDialog(parent)
	, m_language(language)
	, m_targetFps(targetFps)
	, m_preferencesPath(preferencesPath)
{
	// Drop the context-help "?" button on Windows.
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(10, 10, 10, 10);
	root->setSpacing(8);

	// --- General section ---
	m_generalGroup = new QGroupBox(this);
	auto* form = new QFormLayout(m_generalGroup);
	form->setContentsMargins(10, 14, 10, 10);
	form->setSpacing(8);

	// Language: native display names, language code as item data. Row 0 is the
	// "auto" sentinel — it is the shipped default, so it has to be reachable
	// from the UI, and I18n::supportedLanguages() (catalogs only) never yields
	// it. Its text is translated; the concrete languages stay in their own
	// language, which is why only row 0 is touched by Retranslate().
	m_languageCombo = new QComboBox(m_generalGroup);
	m_languageCombo->addItem(QString(), QString(kAutoLanguage));
	for (const auto& lang : I18n::supportedLanguages())
		m_languageCombo->addItem(lang.displayName, lang.code);
	m_languageLabel = new QLabel(m_generalGroup);
	form->addRow(m_languageLabel, m_languageCombo);

	// Target FPS: 0 = unlimited, otherwise frames per second.
	m_fpsCombo = new QComboBox(m_generalGroup);
	m_fpsCombo->addItem(QString(), 0);
	m_fpsCombo->addItem(QString(), 30);
	m_fpsCombo->addItem(QString(), 60);
	m_fpsCombo->addItem(QString(), 120);
	m_fpsLabel = new QLabel(m_generalGroup);
	form->addRow(m_fpsLabel, m_fpsCombo);

	root->addWidget(m_generalGroup);

	// --- Preferences file location ---
	m_pathLabel = new QLabel(this);
	m_pathLabel->setWordWrap(true);
	m_pathLabel->setStyleSheet(QStringLiteral("QLabel { color: #888; }"));
	root->addWidget(m_pathLabel);

	// --- Button row ---
	auto* buttonRow = new QHBoxLayout();
	m_resetBtn = new QPushButton(this);
	buttonRow->addWidget(m_resetBtn);
	buttonRow->addStretch();
	m_closeBtn = new QPushButton(this);
	buttonRow->addWidget(m_closeBtn);
	root->addLayout(buttonRow);

	// --- Wiring ---
	connect(m_languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &PreferencesDialog::OnLanguageChanged);
	connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &PreferencesDialog::OnTargetFpsChanged);
	connect(m_resetBtn, &QPushButton::clicked,
	        this, &PreferencesDialog::OnResetDefaults);
	connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);

	// Retranslate while open when the language changes.
	connect(&I18n::instance(), &I18n::languageChanged,
	        this, &PreferencesDialog::Retranslate);

	// Push current values in, then apply the initial language.
	SyncFrom(m_language, m_targetFps);
	Retranslate();
}

void PreferencesDialog::Retranslate()
{
	auto& i18n = I18n::instance();

	setWindowTitle(i18n.translateCtx("Preferences", "Dialog"));
	m_generalGroup->setTitle(i18n.translate("General"));
	m_languageLabel->setText(i18n.translate("Language"));
	m_fpsLabel->setText(i18n.translate("Target FPS"));

	// Language combo: only the "auto" row is translated — the rest name their
	// own language and must read the same whatever the active one is.
	m_languageCombo->setItemText(kAutoLanguageIndex,
	                             i18n.translate("System default"));

	// FPS combo: item 0 is "Unlimited"; numeric items keep their numbers.
	m_fpsCombo->setItemText(0, i18n.translate("Unlimited"));
	m_fpsCombo->setItemText(1, QStringLiteral("30"));
	m_fpsCombo->setItemText(2, QStringLiteral("60"));
	m_fpsCombo->setItemText(3, QStringLiteral("120"));

	m_pathLabel->setText(i18n.translate("Preferences file") + QStringLiteral(": ")
	                     + m_preferencesPath);
	m_resetBtn->setText(i18n.translate("Reset to Defaults"));
	m_closeBtn->setText(i18n.translate("Close"));
}

void PreferencesDialog::SyncFrom(const QString& language, int targetFps)
{
	m_language = language;
	m_targetFps = targetFps;

	// "auto" is a real row, so findData() resolves it like any other code. An
	// unrecognised code (a catalog that was removed) falls back to "System
	// default" rather than to a specific language.
	const int langIndex = m_languageCombo->findData(language);
	{
		QSignalBlocker block(m_languageCombo);
		m_languageCombo->setCurrentIndex(langIndex >= 0 ? langIndex
		                                                : kAutoLanguageIndex);
	}

	const int fpsIndex = m_fpsCombo->findData(targetFps);
	{
		QSignalBlocker block(m_fpsCombo);
		m_fpsCombo->setCurrentIndex(fpsIndex >= 0 ? fpsIndex : 2);  // default 60
	}
}

void PreferencesDialog::OnLanguageChanged(int index)
{
	emit languageChangeRequested(m_languageCombo->itemData(index).toString());
}

void PreferencesDialog::OnTargetFpsChanged(int index)
{
	emit targetFpsChangeRequested(m_fpsCombo->itemData(index).toInt());
}

void PreferencesDialog::OnResetDefaults()
{
	// Reset to the shipped defaults — "auto" (follow the system language) and
	// 60 FPS — applying + persisting immediately through the same signals as
	// manual edits. Resetting to the *resolved* system code instead would
	// silently pin the preference to today's system language.
	if (m_languageCombo->currentIndex() != kAutoLanguageIndex)
		m_languageCombo->setCurrentIndex(kAutoLanguageIndex);  // → OnLanguageChanged
	else
		emit languageChangeRequested(QString(kAutoLanguage));

	if (m_fpsCombo->currentIndex() != 2)
		m_fpsCombo->setCurrentIndex(2);  // triggers OnTargetFpsChanged
	else
		emit targetFpsChangeRequested(60);
}

} // namespace neurus
