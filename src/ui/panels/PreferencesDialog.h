/**
 * @file PreferencesDialog.h
 * @brief Preferences dialog — app-level settings with instant, live apply.
 *
 * Opened from File → Preferences… (Ctrl+,). All changes apply immediately —
 * picking a language retranslates the entire UI on the spot, picking a target
 * FPS updates the render loop — so there is no OK/Cancel flow, only Close.
 *
 * Architecture:
 * - Pure UI widget: it is seeded with plain values (language code, target
 *   FPS, preferences-file path) and never touches the app-layer Preferences
 *   type — the Application is its sole manager.
 * - Every mutation is emitted as a signal and routed through UIEvents so the
 *   Application applies the change, persists ~/.neurus/preferences.json, and
 *   (for language) triggers the global retranslation.
 * - Connects to I18n::languageChanged() to retranslate itself while open.
 */

#pragma once

#include <QDialog>

class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;

namespace neurus {

class PreferencesDialog : public QDialog
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs the dialog with the current preference values.
	 * @param language        Active language code ("en", "zh_CN").
	 * @param targetFps       Render-loop target FPS (0 = unlimited).
	 * @param preferencesPath Absolute path of ~/.neurus/preferences.json
	 *                        (displayed read-only).
	 * @param parent Parent widget.
	 */
	explicit PreferencesDialog(const QString& language, int targetFps,
	                           const QString& preferencesPath,
	                           QWidget* parent = nullptr);

	/** @brief Re-applies every text in the active language. */
	void Retranslate();

	/**
	 * @brief Re-seeds the controls from the current values (no signals).
	 * @param language  Active language code.
	 * @param targetFps Render-loop target FPS.
	 */
	void SyncFrom(const QString& language, int targetFps);

signals:
	/** @brief Emitted when the user picks a language (code, e.g. "zh_CN"). */
	void languageChangeRequested(const QString& language);

	/** @brief Emitted when the user picks a target FPS (0 = unlimited). */
	void targetFpsChangeRequested(int fps);

private:
	void OnLanguageChanged(int index);
	void OnTargetFpsChanged(int index);
	void OnResetDefaults();

	QString m_language;         ///< Last seeded language code.
	int     m_targetFps = 60;   ///< Last seeded target FPS.
	QString m_preferencesPath;  ///< ~/.neurus/preferences.json (display only).

	QGroupBox*   m_generalGroup  = nullptr;
	QComboBox*   m_languageCombo = nullptr;
	QComboBox*   m_fpsCombo      = nullptr;
	QLabel*      m_languageLabel = nullptr;
	QLabel*      m_fpsLabel      = nullptr;
	QLabel*      m_pathLabel     = nullptr;
	QPushButton* m_resetBtn      = nullptr;
	QPushButton* m_closeBtn      = nullptr;
};

} // namespace neurus
