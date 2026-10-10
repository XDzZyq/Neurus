#pragma once

#include <QMainWindow>
#include <QStringList>
#include "platform/PlatformSurface.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "panels/UIPanel.h"

namespace ads {
class CDockManager;
class CDockWidget;
}

class QAction;
class QLabel;
class QMenu;

namespace neurus {

class PreferencesDialog;

class UIManager : public QMainWindow
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs the main window.
	 *
	 * The Application seeds the UI with plain preference values — the UI
	 * layer never touches the app-layer Preferences type.
	 *
	 * @param language        Active language code ("en", "zh_CN").
	 * @param targetFps       Render-loop target FPS (0 = unlimited).
	 * @param preferencesPath Absolute path of ~/.neurus/preferences.json
	 *                        (displayed by the Preferences dialog).
	 * @param parent Parent widget.
	 */
	explicit UIManager(const QString& language, int targetFps,
	                   const QString& preferencesPath, QWidget* parent = nullptr);
	~UIManager() override;

	/** @brief Returns the Viewport's native window handle for VkSurface creation. */
	NativeWindowHandle getViewportHwnd() const;

	/** @brief Returns viewport widget width in pixels. */
	int getViewportWidth() const;

	/** @brief Returns viewport widget height in pixels. */
	int getViewportHeight() const;

	/**
	 * @brief Refreshes all UIPanel subclasses with the current UIContext.
	 *
	 * Iterates over all registered panels and calls Refresh(ctx) on each.
	 * Called from the newFrame loop after DrawFrame() to keep UI widgets
	 * in sync with Editor/Project state.
	 *
	 * @param ctx Read-only UI context carrying Editor/Project state.
	 */
	void Refresh(const UIContext& ctx);

	/**
	 * @brief Returns typed pointer to a panel by its type.
	 *
	 * Each panel class must provide static constexpr PanelType kType
	 * (e.g. Viewport::kType == PanelType::Viewport).
	 *
	 * @tparam PanelClass UIPanel subclass (Viewport, Outliner, etc.)
	 * @return Non-owning pointer to the panel, or nullptr if not found.
	 */
	template<typename PanelClass>
	PanelClass* GetPanel() const
	{
		auto it = m_panels.find(PanelClass::kType);
		if (it != m_panels.end())
			return qobject_cast<PanelClass*>(it->second);
		return nullptr;
	}

	/**
	 * @brief Returns a dock widget by its PanelType.
	 * @param type PanelType enum value.
	 * @return Non-owning pointer, or nullptr if not found.
	 */
	ads::CDockWidget* GetDock(PanelType type) const
	{
		auto it = m_docks.find(type);
		return (it != m_docks.end()) ? it->second : nullptr;
	}

	/**
	 * @brief Serializes window geometry + ADS dock state into an opaque blob.
	 *
	 * The returned string bundles base64(window geometry) and base64(dock
	 * state) separated by a newline. Application owns persistence.
	 */
	std::string ExportLayout() const;

	/**
	 * @brief Restores window geometry + ADS dock state from a blob produced
	 *        by ExportLayout(). No-op if blob is empty or malformed.
	 */
	void ApplyLayout(const std::string& blob);

protected:
	/**
	 * @brief Locks/unlocks panel tear-off as the window enters/leaves full screen.
	 *
	 * QEvent::WindowStateChange is the only signal Qt gives for macOS's native
	 * full screen (the green button); there is no dedicated notification.
	 */
	void changeEvent(QEvent* event) override;

private:
	void CreateMenus();
	void CreateDocks();
	void RestoreDefaultLayout();

	/**
	 * @brief Creates a dock for @p panel with a translated title and a stable id.
	 *
	 * ads::CDockWidget's constructor copies the title into objectName, and ADS
	 * serializes layouts by objectName — so constructing a dock from translated
	 * text alone makes saved layouts language-dependent. Every dock MUST be
	 * created through this helper (or set its objectName explicitly) so the
	 * title stays translatable while the serialization key stays stable.
	 */
	ads::CDockWidget* MakeDock(UIPanel* panel);

	/**
	 * @brief Re-docks any dock the restored layout did not claim.
	 *
	 * ADS silently skips unknown objectNames when restoring and leaves those
	 * docks with no dock area, so a foreign or stale blob yields an empty
	 * window with no error. Called after ApplyLayout() to make that recoverable.
	 */
	void RepairOrphanedDocks();

	/**
	 * @brief Keeps panels docked while the window is full screen.
	 *
	 * A torn-off panel is a separate top-level window, and a separate window
	 * cannot be used inside another window's full-screen Space: macOS gives ADS's
	 * plain Qt::Window floating container FullScreenPrimary collection behaviour,
	 * and the window server then refuses to let the user move it. Rather than
	 * fight the platform for a window that has nowhere sensible to live, full
	 * screen simply forbids tearing off.
	 *
	 * Only DockWidgetFloatable is locked, so dragging a panel to a *different
	 * dock position* keeps working — it is only the "drop it outside any dock
	 * area" gesture that now snaps back. ADS masks locked features out of
	 * CDockWidget::features() without touching the per-widget flags, so leaving
	 * full screen restores whatever each panel had.
	 *
	 * @param lock true on entering full screen, false on leaving.
	 */
	void SetFloatingLocked(bool lock);

	/**
	 * @brief Re-docks every panel that is currently in its own window.
	 *
	 * Called when entering full screen: an already-floating panel would other-
	 * wise be stranded on the desktop Space, visible only by leaving full screen.
	 * Panels return to their default areas — ADS has no "undo the tear-off"
	 * that would restore the exact prior position.
	 */
	void DockFloatingPanels();

	/**
	 * @brief Re-applies every user-visible string in the active language:
	 *        menu bar, dock titles, panel texts, Preferences dialog.
	 *        Connected to I18n::languageChanged() and called once at startup.
	 */
	void RetranslateAll();

	/** @brief Shows (or creates) the Preferences dialog. */
	void OpenPreferences();

	/** @brief Populates the Undo submenu with the applied-operation stack. */
	void PopulateUndoMenu();
	/** @brief Populates the Redo submenu with the undone-operation stack. */
	void PopulateRedoMenu();

	ads::CDockManager* win_dockManager = nullptr;

	/// True while full screen has DockWidgetFloatable locked; guards against
	/// re-applying on the WindowStateChange events full screen also emits for
	/// maximize/minimize.
	bool m_floatingLocked = false;

	/// View > Full Screen toggle; changeEvent() keeps its check mark in step so
	/// it follows full screen however that state was entered.
	QAction* m_fullScreenAction = nullptr;

	// --- Panel registry (raw pointers; Qt parent-child manages lifetime) ---
	std::map<PanelType, QWidget*> m_panels;

	// --- Dock registry (raw pointers; CDockManager owns via Qt parent-child) ---
	std::map<PanelType, ads::CDockWidget*> m_docks;

	// --- Menu translation registry: (action, i18n key) pairs ---
	std::vector<std::pair<QAction*, const char*>> m_menuItems;

	// --- Edit-menu Undo/Redo submenus (view-only stack lists) ---
	QMenu*                m_undoMenu = nullptr; ///< Expands to the applied-op stack.
	QMenu*                m_redoMenu = nullptr; ///< Expands to the undone-op stack.
	std::vector<QAction*> m_undoItems;          ///< Rows currently shown in Undo menu.
	std::vector<QAction*> m_redoItems;          ///< Rows currently shown in Redo menu.
	QStringList           m_undoLabels;         ///< Applied ops, oldest → newest.
	QStringList           m_redoLabels;         ///< Undone ops, in replay order.

	// --- Preferences dialog (lazy, non-dock panel) ---
	// The dialog is seeded with plain values (the Application is the sole
	// owner of the app-layer Preferences type).
	QString            m_currentLanguage;    ///< Cached active language code.
	int                m_targetFps = 60;     ///< Cached render-loop target FPS.
	QString            m_preferencesPath;    ///< ~/.neurus/preferences.json.
	PreferencesDialog* m_preferencesDialog = nullptr; ///< Lazy-created; Qt parent owns.

	// --- Texture Viewer placeholder dock (not a UIPanel) ---
	ads::CDockWidget* m_textureDock  = nullptr;
	QLabel*           m_textureLabel = nullptr;
};

} // namespace neurus
