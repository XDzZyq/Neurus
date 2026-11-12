#include "panels/PreferencesDialog.h"

#include "ui/utils/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
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
                                     const ScriptPreferencesUi& script,
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
	// "auto" sentinel; its text is translated, while the concrete languages
	// stay in their own language - hence only row 0 is touched by Retranslate().
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

	// --- Script sandbox section (host policy for external runs) ---
	m_sandboxGroup = new QGroupBox(this);
	auto* sandboxForm = new QFormLayout(m_sandboxGroup);
	sandboxForm->setContentsMargins(10, 14, 10, 10);
	sandboxForm->setSpacing(8);

	m_readRootsEdit = new QLineEdit(m_sandboxGroup);
	m_writeRootsEdit = new QLineEdit(m_sandboxGroup);
	m_blockedModulesEdit = new QLineEdit(m_sandboxGroup);
	m_readRootsLabel = new QLabel(m_sandboxGroup);
	m_writeRootsLabel = new QLabel(m_sandboxGroup);
	m_blockedModulesLabel = new QLabel(m_sandboxGroup);
	sandboxForm->addRow(m_readRootsLabel, m_readRootsEdit);
	sandboxForm->addRow(m_writeRootsLabel, m_writeRootsEdit);
	sandboxForm->addRow(m_blockedModulesLabel, m_blockedModulesEdit);

	m_allowNetworkCheck = new QCheckBox(m_sandboxGroup);
	m_allowSubprocessCheck = new QCheckBox(m_sandboxGroup);
	m_allowCtypesCheck = new QCheckBox(m_sandboxGroup);
	sandboxForm->addRow(m_allowNetworkCheck);
	sandboxForm->addRow(m_allowSubprocessCheck);
	sandboxForm->addRow(m_allowCtypesCheck);

	m_timeLimitLabel = new QLabel(m_sandboxGroup);
	m_timeLimitSpin = new QSpinBox(m_sandboxGroup);
	m_timeLimitSpin->setRange(0, 3600);
	m_memoryLimitLabel = new QLabel(m_sandboxGroup);
	m_memoryLimitSpin = new QSpinBox(m_sandboxGroup);
	m_memoryLimitSpin->setRange(0, 65536);
	sandboxForm->addRow(m_timeLimitLabel, m_timeLimitSpin);
	sandboxForm->addRow(m_memoryLimitLabel, m_memoryLimitSpin);
	m_readRootsEdit->setObjectName(QStringLiteral("scriptReadRoots"));
	m_writeRootsEdit->setObjectName(QStringLiteral("scriptWriteRoots"));
	m_blockedModulesEdit->setObjectName(QStringLiteral("scriptBlockedModules"));
	m_allowNetworkCheck->setObjectName(QStringLiteral("scriptAllowNetwork"));
	m_allowSubprocessCheck->setObjectName(QStringLiteral("scriptAllowSubprocess"));
	m_allowCtypesCheck->setObjectName(QStringLiteral("scriptAllowCtypes"));
	m_timeLimitSpin->setObjectName(QStringLiteral("scriptTimeLimit"));
	m_memoryLimitSpin->setObjectName(QStringLiteral("scriptMemoryLimit"));
	root->addWidget(m_sandboxGroup);

	// --- Script preload section ---
	m_preloadGroup = new QGroupBox(this);
	auto* preloadForm = new QFormLayout(m_preloadGroup);
	preloadForm->setContentsMargins(10, 14, 10, 10);
	preloadForm->setSpacing(8);
	m_preloadEdit = new QLineEdit(m_preloadGroup);
	m_preloadEdit->setObjectName(QStringLiteral("scriptPreload"));
	m_preloadLabel = new QLabel(m_preloadGroup);
	preloadForm->addRow(m_preloadLabel, m_preloadEdit);
	root->addWidget(m_preloadGroup);

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
	connect(m_readRootsEdit, &QLineEdit::editingFinished,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_writeRootsEdit, &QLineEdit::editingFinished,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_blockedModulesEdit, &QLineEdit::editingFinished,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_preloadEdit, &QLineEdit::editingFinished,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_allowNetworkCheck, &QCheckBox::toggled,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_allowSubprocessCheck, &QCheckBox::toggled,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_allowCtypesCheck, &QCheckBox::toggled,
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_timeLimitSpin, QOverload<int>::of(&QSpinBox::valueChanged),
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_memoryLimitSpin, QOverload<int>::of(&QSpinBox::valueChanged),
	        this, &PreferencesDialog::OnScriptFieldChanged);
	connect(m_resetBtn, &QPushButton::clicked,
	        this, &PreferencesDialog::OnResetDefaults);
	connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);

	// Retranslate while open when the language changes.
	connect(&I18n::instance(), &I18n::languageChanged,
	        this, &PreferencesDialog::Retranslate);

	// Push current values in, then apply the initial language.
	SyncFrom(m_language, m_targetFps, script);
	Retranslate();
}

void PreferencesDialog::Retranslate()
{
	auto& i18n = I18n::instance();

	setWindowTitle(i18n.translateCtx("Preferences", "Dialog"));
	m_generalGroup->setTitle(i18n.translate("General"));
	m_languageLabel->setText(i18n.translate("Language"));
	m_fpsLabel->setText(i18n.translate("Target FPS"));

	// Language combo: only the "auto" row is translated; the rest name their
	// own language and read the same whatever the active one is.
	m_languageCombo->setItemText(kAutoLanguageIndex,
	                             i18n.translate("System default"));

	// FPS combo: item 0 is "Unlimited"; numeric items keep their numbers.
	m_fpsCombo->setItemText(0, i18n.translate("Unlimited"));
	m_fpsCombo->setItemText(1, QStringLiteral("30"));
	m_fpsCombo->setItemText(2, QStringLiteral("60"));
	m_fpsCombo->setItemText(3, QStringLiteral("120"));

	m_sandboxGroup->setTitle(i18n.translate("Script Sandbox"));
	m_readRootsLabel->setText(i18n.translate("Read roots"));
	m_writeRootsLabel->setText(i18n.translate("Write roots"));
	m_blockedModulesLabel->setText(i18n.translate("Blocked modules"));
	m_allowNetworkCheck->setText(i18n.translate("Allow network"));
	m_allowSubprocessCheck->setText(i18n.translate("Allow subprocess"));
	m_allowCtypesCheck->setText(i18n.translate("Allow ctypes"));
	m_timeLimitLabel->setText(i18n.translate("Time limit (s)"));
	m_memoryLimitLabel->setText(i18n.translate("Memory limit (MB)"));
	m_readRootsEdit->setPlaceholderText(
		i18n.translateCtx("Semicolon-separated; empty uses the project directory", "Placeholder"));
	m_writeRootsEdit->setPlaceholderText(
		i18n.translateCtx("Semicolon-separated; empty uses the project directory", "Placeholder"));
	m_blockedModulesEdit->setPlaceholderText(
		i18n.translateCtx("Semicolon-separated module names", "Placeholder"));
	m_preloadGroup->setTitle(i18n.translate("Script Preload"));
	m_preloadLabel->setText(i18n.translate("Preload packages"));
	m_preloadEdit->setPlaceholderText(
		i18n.translateCtx("Comma- or semicolon-separated package names", "Placeholder"));

	m_pathLabel->setText(i18n.translate("Preferences file") + QStringLiteral(": ")
	                     + m_preferencesPath);
	m_resetBtn->setText(i18n.translate("Reset to Defaults"));
	m_closeBtn->setText(i18n.translate("Close"));
}

void PreferencesDialog::SyncFrom(const QString& language, int targetFps,
                                 const ScriptPreferencesUi& script)
{
	m_language = language;
	m_targetFps = targetFps;

	// "auto" is a real row, so findData() resolves it like any other code; an
	// unrecognised code falls back to the system-default row rather than to a
	// specific language.
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

	m_syncing = true;
	m_readRootsEdit->setText(script.readRoots);
	m_writeRootsEdit->setText(script.writeRoots);
	m_blockedModulesEdit->setText(script.blockedModules);
	m_preloadEdit->setText(script.preload);
	m_allowNetworkCheck->setChecked(script.allowNetwork);
	m_allowSubprocessCheck->setChecked(script.allowSubprocess);
	m_allowCtypesCheck->setChecked(script.allowCtypes);
	m_timeLimitSpin->setValue(script.timeLimitSeconds);
	m_memoryLimitSpin->setValue(script.memoryLimitMb);
	m_syncing = false;
}

ScriptPreferencesUi PreferencesDialog::ScriptPreferences() const
{
	ScriptPreferencesUi script;
	script.readRoots = m_readRootsEdit->text();
	script.writeRoots = m_writeRootsEdit->text();
	script.blockedModules = m_blockedModulesEdit->text();
	script.preload = m_preloadEdit->text();
	script.allowNetwork = m_allowNetworkCheck->isChecked();
	script.allowSubprocess = m_allowSubprocessCheck->isChecked();
	script.allowCtypes = m_allowCtypesCheck->isChecked();
	script.timeLimitSeconds = m_timeLimitSpin->value();
	script.memoryLimitMb = m_memoryLimitSpin->value();
	return script;
}

void PreferencesDialog::OnScriptFieldChanged()
{
	if (!m_syncing)
	{
		emit scriptPreferencesChanged();
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
	// Reset to the shipped defaults - "auto" and 60 FPS - applying and
	// persisting through the same signals as manual edits. Resetting to the
	// resolved system code would silently pin today's system language.
	if (m_languageCombo->currentIndex() != kAutoLanguageIndex)
		m_languageCombo->setCurrentIndex(kAutoLanguageIndex);  // → OnLanguageChanged
	else
		emit languageChangeRequested(QString(kAutoLanguage));

	if (m_fpsCombo->currentIndex() != 2)
		m_fpsCombo->setCurrentIndex(2);  // triggers OnTargetFpsChanged
	else
		emit targetFpsChangeRequested(60);

	m_syncing = true;
	m_readRootsEdit->clear();
	m_writeRootsEdit->clear();
	m_blockedModulesEdit->clear();
	m_preloadEdit->clear();
	m_allowNetworkCheck->setChecked(false);
	m_allowSubprocessCheck->setChecked(false);
	m_allowCtypesCheck->setChecked(false);
	m_timeLimitSpin->setValue(30);
	m_memoryLimitSpin->setValue(0);
	m_syncing = false;
	emit scriptPreferencesChanged();
}

} // namespace neurus
