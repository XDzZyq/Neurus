/**
 * @file PreferencesDialog.h
 * @brief Preferences dialog — app-level settings with instant, live apply.
 *
 * Opened from File > Preferences... (Ctrl+,). All changes apply immediately -
 * picking a language retranslates the entire UI on the spot, picking a target
 * FPS updates the render loop - so there is no OK/Cancel flow, only Close.
 * Every mutation is emitted as a signal, so the consumer applies and persists
 * the change.
 */

#pragma once

#include <QDialog>

#include "ui/panels/ScriptPreferences.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace neurus {

class PreferencesDialog : public QDialog
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs the dialog with the current preference values.
	 * @param language        Active language code ("en", "zh_CN").
	 * @param targetFps       Render-loop target FPS (0 = unlimited).
	 * @param script          Script sandbox and preload values.
	 * @param preferencesPath Path of the preferences file, shown read-only.
	 * @param parent Parent widget.
	 */
	explicit PreferencesDialog(const QString& language, int targetFps,
	                           const ScriptPreferencesUi& script,
	                           const QString& preferencesPath,
	                           QWidget* parent = nullptr);

	/** @brief Re-applies every text in the active language. */
	void Retranslate();

	/**
	 * @brief Re-seeds the controls from the current values (no signals).
	 * @param language  Active language code.
	 * @param targetFps Render-loop target FPS.
	 * @param script    Script sandbox and preload values.
	 */
	void SyncFrom(const QString& language, int targetFps,
	              const ScriptPreferencesUi& script);

	/// @return The script values currently shown in the dialog.
	ScriptPreferencesUi ScriptPreferences() const;

signals:
	/** @brief Emitted when the user picks a language (code, e.g. "zh_CN"). */
	void languageChangeRequested(const QString& language);

	/** @brief Emitted when the user picks a target FPS (0 = unlimited). */
	void targetFpsChangeRequested(int fps);

	/** @brief Emitted when any Script Sandbox or Script Preload field changes. */
	void scriptPreferencesChanged();

private:
	void OnLanguageChanged(int index);
	void OnTargetFpsChanged(int index);
	void OnResetDefaults();
	void OnScriptFieldChanged();

	QString m_language;         ///< Last seeded language code.
	int     m_targetFps = 60;   ///< Last seeded target FPS.
	QString m_preferencesPath;  ///< ~/.neurus/preferences.json (display only).
	bool    m_syncing = false;  ///< True while SyncFrom suppresses change signals.

	QGroupBox*   m_generalGroup      = nullptr;
	QComboBox*   m_languageCombo     = nullptr;
	QComboBox*   m_fpsCombo          = nullptr;
	QLabel*      m_languageLabel     = nullptr;
	QLabel*      m_fpsLabel          = nullptr;
	QLabel*      m_pathLabel         = nullptr;
	QGroupBox*   m_sandboxGroup      = nullptr;
	QLineEdit*   m_readRootsEdit     = nullptr;
	QLineEdit*   m_writeRootsEdit    = nullptr;
	QLineEdit*   m_blockedModulesEdit = nullptr;
	QCheckBox*   m_allowNetworkCheck = nullptr;
	QCheckBox*   m_allowSubprocessCheck = nullptr;
	QCheckBox*   m_allowCtypesCheck  = nullptr;
	QSpinBox*    m_timeLimitSpin     = nullptr;
	QSpinBox*    m_memoryLimitSpin   = nullptr;
	QLabel*      m_readRootsLabel    = nullptr;
	QLabel*      m_writeRootsLabel   = nullptr;
	QLabel*      m_blockedModulesLabel = nullptr;
	QLabel*      m_timeLimitLabel    = nullptr;
	QLabel*      m_memoryLimitLabel  = nullptr;
	QGroupBox*   m_preloadGroup      = nullptr;
	QLineEdit*   m_preloadEdit       = nullptr;
	QLabel*      m_preloadLabel      = nullptr;
	QPushButton* m_resetBtn          = nullptr;
	QPushButton* m_closeBtn          = nullptr;
};

} // namespace neurus
