#include "UIManager.h"
#include "Icons.h"
#include "panels/LogPanel.h"
#include "panels/Outliner.h"
#include "panels/PreferencesDialog.h"
#include "panels/ProfilingPanel.h"
#include "panels/PropertyPanel.h"
#include "panels/RenderConfigPanel.h"
#include "panels/ShaderEditorPanel.h"
#include "panels/Viewport.h"
#include "UIContext.h"
#include "ui/utils/I18n.h"

#include "core/Log.h"
#include "editor/events/UIEvents.h"
#include "editor/operations/HistoryView.h"

#include <QApplication>
#include <QAction>
#include <QEvent>
#include <QFileDialog>
#include <QFont>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>

#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
#include <FloatingDockContainer.h>

#include <memory>

namespace neurus {

// =========================================================================
// Constructor / Destructor
// =========================================================================

UIManager::UIManager(const QString& language, int targetFps,
                       const QString& preferencesPath, QWidget* parent)
	: QMainWindow(parent)
	, m_currentLanguage(language)
	, m_targetFps(targetFps)
	, m_preferencesPath(preferencesPath)
{
	Icons::Initialize();

	setWindowTitle("Neurus");
	resize(1600, 900);

	// Disable opaque splitter resize for better Vulkan window container behavior
	ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, false);
	ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);

	win_dockManager = new ads::CDockManager(this);

	CreateDocks();
	CreateMenus();

	// Language switch (e.g. from Preferences) retranslates the whole window
	// immediately — no restart needed.
	connect(&I18n::instance(), &I18n::languageChanged,
	        this, &UIManager::RetranslateAll);

	// Apply the startup language (menus were built with English literals).
	RetranslateAll();
}

UIManager::~UIManager()
{
	// Release panel/dock pointers before Qt parent-child cleanup destroys them.
	m_panels.clear();
	m_docks.clear();
}

// =========================================================================
// Viewport accessors
// =========================================================================

NativeWindowHandle UIManager::getViewportHwnd() const
{
	auto* vp = GetPanel<Viewport>();
	return vp ? vp->hwnd() : nullptr;
}

int UIManager::getViewportWidth() const
{
	auto* vp = GetPanel<Viewport>();
	return vp ? vp->width() : 0;
}

int UIManager::getViewportHeight() const
{
	auto* vp = GetPanel<Viewport>();
	return vp ? vp->height() : 0;
}

// =========================================================================
// Refresh – push UIContext to all registered panels
// =========================================================================

void UIManager::Refresh(const UIContext& ctx)
{
	// Cache the latest undo/redo labels so the Edit submenus can display the
	// stacks on demand (the snapshot pointer is only valid for this call).
	if (const auto* history = static_cast<const HistoryView*>(ctx.history))
	{
		m_undoLabels.clear();
		for (const auto& label : history->undo)
			m_undoLabels.push_back(QString::fromStdString(label));

		m_redoLabels.clear();
		for (const auto& label : history->redo)
			m_redoLabels.push_back(QString::fromStdString(label));

		// Grey out an empty submenu, matching how Undo/Redo normally disable.
		if (m_undoMenu)
			m_undoMenu->menuAction()->setEnabled(!m_undoLabels.isEmpty());
		if (m_redoMenu)
			m_redoMenu->menuAction()->setEnabled(!m_redoLabels.isEmpty());
	}

	for (auto& [type, widget] : m_panels)
	{
		auto* panel = qobject_cast<UIPanel*>(widget);
		panel->Refresh(ctx);
	}
}

// =========================================================================
// Edit menu Undo/Redo submenus — populate the stack list before showing
// =========================================================================

void UIManager::PopulateUndoMenu()
{
	for (QAction* item : m_undoItems)
	{
		m_undoMenu->removeAction(item);
		delete item;
	}
	m_undoItems.clear();

	// Newest first: the top row is the next Ctrl+Z target. View-only (disabled).
	for (auto it = m_undoLabels.crbegin(); it != m_undoLabels.crend(); ++it)
	{
		auto* item = new QAction(*it, m_undoMenu);
		item->setEnabled(false);
		m_undoMenu->addAction(item);
		m_undoItems.push_back(item);
	}
}

void UIManager::PopulateRedoMenu()
{
	for (QAction* item : m_redoItems)
	{
		m_redoMenu->removeAction(item);
		delete item;
	}
	m_redoItems.clear();

	// Replay order: the top row is the next Ctrl+Y target. View-only (disabled).
	for (const QString& label : m_redoLabels)
	{
		auto* item = new QAction(label, m_redoMenu);
		item->setEnabled(false);
		m_redoMenu->addAction(item);
		m_redoItems.push_back(item);
	}
}

// =========================================================================
// Menus
// =========================================================================

void UIManager::CreateMenus()
{
	auto& i18n = I18n::instance();

	// Translated-menu helpers: create a menu/action with the current-language
	// text and register (action, key) so RetranslateAll() can re-apply texts
	// on language switches.
	auto addMenu = [this, &i18n](auto* parent, const char* key) -> QMenu* {
		auto* menu = parent->addMenu(i18n.translate(key));
		m_menuItems.emplace_back(menu->menuAction(), key);
		return menu;
	};
	auto trAction = [this, &i18n](auto* parent, const char* key) -> QAction* {
		auto* action = parent->addAction(i18n.translate(key));
		m_menuItems.emplace_back(action, key);
		return action;
	};

	auto* fileMenu = addMenu(menuBar(), N_("&File"));

	auto* newAction = trAction(fileMenu, N_("&New"));
	newAction->setShortcut(QKeySequence("Ctrl+N"));
	connect(newAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestProjectNew();
	});

	auto* openAction = trAction(fileMenu, N_("&Open..."));
	openAction->setShortcut(QKeySequence("Ctrl+O"));
	connect(openAction, &QAction::triggered, []() {
		QString path = QFileDialog::getOpenFileName(
			nullptr, I18n::instance().translateCtx("Open Project", "Dialog"), QString(),
			I18n::instance().translateCtx("Neurus Project (*.neurus.json)", "Dialog"));
		if (!path.isEmpty())
			neurus::UIEvents::instance().requestProjectOpen(path);
	});

	auto* saveAction = trAction(fileMenu, N_("&Save"));
	saveAction->setShortcut(QKeySequence("Ctrl+S"));
	connect(saveAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestProjectSave();
	});

	auto* saveAsAction = trAction(fileMenu, N_("Save &As..."));
	saveAsAction->setShortcut(QKeySequence("Ctrl+Shift+S"));
	connect(saveAsAction, &QAction::triggered, []() {
		QString path = QFileDialog::getSaveFileName(
			nullptr, I18n::instance().translateCtx("Save Project As", "Dialog"), QString(),
			I18n::instance().translateCtx("Neurus Project (*.neurus.json)", "Dialog"));
		if (!path.isEmpty())
			neurus::UIEvents::instance().requestProjectSaveAs(path);
	});

	fileMenu->addSeparator();

	auto* prefsAction = trAction(fileMenu, N_("&Preferences..."));
	prefsAction->setShortcut(QKeySequence::Preferences);
	connect(prefsAction, &QAction::triggered, this, &UIManager::OpenPreferences);

	fileMenu->addSeparator();

	auto* exitAction = trAction(fileMenu, N_("E&xit"));
	exitAction->setShortcut(QKeySequence::Quit);
	connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

	auto* viewMenu = addMenu(menuBar(), N_("&View"));

	auto* resetLayoutAction = trAction(viewMenu, N_("Restore &Default Layout"));
	connect(resetLayoutAction, &QAction::triggered, this, &UIManager::RestoreDefaultLayout);

	auto* editMenu = addMenu(menuBar(), N_("&Edit"));

	// Undo/Redo are expandable submenus that reveal their stacks (like "Add").
	// The actual trigger actions live on the window so Ctrl+Z / Ctrl+Y keep
	// working without a clickable menu action.
	auto* undoTrigger = new QAction(this);
	undoTrigger->setShortcut(QKeySequence::Undo);
	connect(undoTrigger, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestUndo();
	});
	addAction(undoTrigger);

	auto* redoTrigger = new QAction(this);
	// QKeySequence::Redo maps to Ctrl+Y on Windows; also bind Ctrl+Shift+Z so
	// the common cross-platform redo chord works everywhere (Issue #43).
	redoTrigger->setShortcuts({ QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z") });
	connect(redoTrigger, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestRedo();
	});
	addAction(redoTrigger);

	m_undoMenu = addMenu(editMenu, N_("&Undo"));
	connect(m_undoMenu, &QMenu::aboutToShow, this, &UIManager::PopulateUndoMenu);

	m_redoMenu = addMenu(editMenu, N_("&Redo"));
	connect(m_redoMenu, &QMenu::aboutToShow, this, &UIManager::PopulateRedoMenu);

	editMenu->addSeparator();

	auto* addMenu2 = addMenu(editMenu, N_("&Add"));

	auto* meshAction = trAction(addMenu2, N_("&Mesh..."));
	meshAction->setShortcut(QKeySequence("Ctrl+Shift+M"));
	connect(meshAction, &QAction::triggered, []() {
		QString path = QFileDialog::getOpenFileName(
			nullptr, I18n::instance().translateCtx("Import Mesh", "Dialog"), QString(),
			I18n::instance().translateCtx("OBJ Files (*.obj)", "Dialog"));
		if (!path.isEmpty())
			neurus::UIEvents::instance().requestMeshImport(path);
	});

	auto* cameraAction = trAction(addMenu2, N_("&Camera"));
	connect(cameraAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestCameraAdd();
	});

	auto* lightSubmenu = addMenu(addMenu2, N_("&Light"));

	auto* pointLightAction = trAction(lightSubmenu, N_("&Point Light"));
	connect(pointLightAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestLightAdd();
	});

	auto* sunLightAction = trAction(lightSubmenu, N_("&Sun Light"));
	connect(sunLightAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestSunLightAdd();
	});

	auto* spotLightAction = trAction(lightSubmenu, N_("Spo&t Light"));
	connect(spotLightAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestSpotLightAdd();
	});

	auto* toolsMenu = addMenu(menuBar(), N_("&Tools"));

	auto* screenshotAction = trAction(toolsMenu, N_("Take &Screenshot"));
	screenshotAction->setShortcut(QKeySequence("F12"));
	connect(screenshotAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestScreenshot();
	});

	auto* screenshotAllAction = trAction(toolsMenu, N_("Screenshot All &Passes"));
	screenshotAllAction->setShortcut(QKeySequence("Ctrl+F12"));
	connect(screenshotAllAction, &QAction::triggered, []() {
		neurus::UIEvents::instance().requestScreenshotAll();
	});

	auto* helpMenu = addMenu(menuBar(), N_("&Help"));
	auto* aboutAction = trAction(helpMenu, N_("&About Neurus"));
	connect(aboutAction, &QAction::triggered, this, [this]() {
		QMessageBox::about(this,
			I18n::instance().translateCtx("About Neurus", "Dialog"),
			QString("<h2>Neurus</h2>"
			        "<p>%1</p>"
			        "<p>%2</p>")
				.arg(I18n::instance().translateCtx(
					"A C++20 Vulkan-HPP 1.4 real-time renderer.", "Dialog"))
				.arg(I18n::instance().translateCtx(
					"Version 0.1.0 (Triangle MVP)", "Dialog")));
	});
}

// =========================================================================
// Docks
// =========================================================================

namespace {

/**
 * @brief Fallback dock area for a panel, used by RepairOrphanedDocks().
 *
 * Mirrors the arrangement CreateDocks() builds. CreateDocks() keeps its own
 * explicit areas because it also tabs panels onto specific sibling areas;
 * this is only the "put it somewhere sensible" placement for a dock the
 * restored layout failed to claim.
 */
ads::DockWidgetArea DefaultDockArea(PanelType type)
{
	switch (type)
	{
	case PanelType::Viewport:
	case PanelType::Outliner:
	case PanelType::ShaderEditor:   return ads::LeftDockWidgetArea;
	case PanelType::PropertyPanel:
	case PanelType::RenderConfig:   return ads::RightDockWidgetArea;
	case PanelType::Profiling:
	case PanelType::Log:            return ads::BottomDockWidgetArea;
	case PanelType::Count:          break;
	}
	return ads::CenterDockWidgetArea;
}

} // namespace

void UIManager::changeEvent(QEvent* event)
{
	QMainWindow::changeEvent(event);

	if (event->type() != QEvent::WindowStateChange)
		return;

	// Fires for maximize and minimize too, so compare against the tracked state
	// instead of acting on every transition.
	const bool fullScreen = isFullScreen();
	if (fullScreen == m_floatingLocked)
		return;

	if (fullScreen)
		DockFloatingPanels();
	SetFloatingLocked(fullScreen);
}

void UIManager::SetFloatingLocked(bool lock)
{
	if (!win_dockManager)
		return;

	// Lock Floatable only. Movable stays available so a panel can still be
	// dragged to a different dock position; ADS then finds no floatable content
	// at drop time (FloatingDragPreview::createFloatingWidget) and the panel
	// snaps back instead of becoming a window.
	win_dockManager->lockDockWidgetFeaturesGlobally(
		lock ? ads::CDockWidget::DockWidgetFloatable
		     : ads::CDockWidget::NoDockWidgetFeatures);
	m_floatingLocked = lock;
}

void UIManager::DockFloatingPanels()
{
	if (!win_dockManager)
		return;

	// Collect first: addDockWidget() empties the floating container and ADS
	// deletes it, which would invalidate the list mid-iteration.
	std::vector<ads::CDockWidget*> stranded;
	for (ads::CFloatingDockContainer* container : win_dockManager->floatingWidgets())
	{
		if (!container)
			continue;
		for (ads::CDockWidget* dock : container->dockWidgets())
			if (dock)
				stranded.push_back(dock);
	}
	if (stranded.empty())
		return;

	QStringList docked;
	for (ads::CDockWidget* dock : stranded)
	{
		// Reverse-lookup the panel type to reuse the same fallback areas
		// RepairOrphanedDocks() uses; the texture dock is not a UIPanel.
		auto area = ads::BottomDockWidgetArea;
		for (const auto& [type, registered] : m_docks)
			if (registered == dock)
			{
				area = DefaultDockArea(type);
				break;
			}

		win_dockManager->addDockWidget(area, dock);
		docked << dock->objectName();
	}

	NEURUS_LOG("[UIManager] Full screen cannot host separate panel windows; "
	           "re-docked: " << docked.join(QStringLiteral(", ")).toStdString());
}

ads::CDockWidget* UIManager::MakeDock(UIPanel* panel)
{
	// The title is translated (display only); the objectName is the stable
	// serialization key ADS saves and restores by. CDockWidget's ctor derives
	// objectName from the title, so it MUST be overwritten here — otherwise a
	// layout saved in one language cannot be restored in another.
	auto* dock = new ads::CDockWidget(win_dockManager, panel->PanelName());
	dock->setObjectName(panel->PanelId());
	return dock;
}

void UIManager::CreateDocks()
{
	// --- Viewport (MUST be created FIRST - ADS central widget requirement) ---
	auto viewport = std::make_unique<Viewport>();
	viewport->resize(800, 600);
	viewport->winId();  // Force native window handle creation
	NativeWindowHandle newHwnd = viewport->hwnd();

	auto* viewportDock = MakeDock(viewport.get());
	viewportDock->setWidget(viewport.get(), ads::CDockWidget::ForceNoScrollArea);
	viewportDock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
	win_dockManager->addDockWidget(ads::LeftDockWidgetArea, viewportDock);

	// Transfer panel ownership to CDockWidget (Qt parent-child).
	// After setWidget(), the dock owns the panel; release unique_ptr to avoid double-delete.
	m_panels[PanelType::Viewport] = viewport.release();
	m_docks[PanelType::Viewport] = viewportDock;

	// --- Left: Shader Editor ---
	auto shaderEditor = std::make_unique<ShaderEditorPanel>();
	auto* shaderDock = MakeDock(shaderEditor.get());
	shaderDock->setWidget(shaderEditor.get(), ads::CDockWidget::ForceNoScrollArea);
	shaderDock->resize(280, 300);
	shaderDock->setMinimumSize(200, 200);
	win_dockManager->addDockWidget(ads::LeftDockWidgetArea, shaderDock);
	m_panels[PanelType::ShaderEditor] = shaderEditor.release();
	m_docks[PanelType::ShaderEditor] = shaderDock;

	// --- Left: Outliner ---
	auto outliner = std::make_unique<Outliner>();
	auto* outlinerDock = MakeDock(outliner.get());
	outlinerDock->setWidget(outliner.get());
	outlinerDock->resize(280, 300);
	outlinerDock->setMinimumSize(200, 200);
	win_dockManager->addDockWidget(ads::LeftDockWidgetArea, outlinerDock);
	m_panels[PanelType::Outliner] = outliner.release();
	m_docks[PanelType::Outliner] = outlinerDock;

	// --- Right: Property Panel ---
	auto propertyPanel = std::make_unique<PropertyPanel>();
	auto* propDock = MakeDock(propertyPanel.get());
	propDock->setWidget(propertyPanel.get());
	propDock->resize(280, 300);
	propDock->setMinimumSize(200, 200);
	win_dockManager->addDockWidget(ads::RightDockWidgetArea, propDock, outlinerDock->dockAreaWidget());
	m_panels[PanelType::PropertyPanel] = propertyPanel.release();
	m_docks[PanelType::PropertyPanel] = propDock;

	// --- Right: Render Config ---
	auto renderConfigPanel = std::make_unique<RenderConfigPanel>();
	auto* configDock = MakeDock(renderConfigPanel.get());
	configDock->setWidget(renderConfigPanel.get(), ads::CDockWidget::ForceNoScrollArea);
	configDock->resize(280, 400);
	configDock->setMinimumSize(220, 300);
	win_dockManager->addDockWidget(ads::RightDockWidgetArea, configDock, outlinerDock->dockAreaWidget());
	m_panels[PanelType::RenderConfig] = renderConfigPanel.release();
	m_docks[PanelType::RenderConfig] = configDock;

	// --- Bottom: Profiling ---
	auto profilingPanel = std::make_unique<ProfilingPanel>();
	auto* profilingDock = MakeDock(profilingPanel.get());
	profilingDock->setWidget(profilingPanel.get(), ads::CDockWidget::ForceNoScrollArea);
	profilingDock->resize(640, 220);
	profilingDock->setMinimumSize(320, 140);
	win_dockManager->addDockWidget(ads::BottomDockWidgetArea, profilingDock);
	m_panels[PanelType::Profiling] = profilingPanel.release();
	m_docks[PanelType::Profiling] = profilingDock;

	// --- Bottom: Texture Viewer (placeholder, not a UIPanel) ---
	m_textureLabel = new QLabel(I18n::instance().translateCtx("Texture Viewer", "Dock"));
	m_textureLabel->setAlignment(Qt::AlignCenter);
	QFont font = m_textureLabel->font();
	font.setPointSize(14);
	m_textureLabel->setFont(font);
	auto* textureWrap = new QWidget();
	auto* textureLayout = new QVBoxLayout(textureWrap);
	textureLayout->addWidget(m_textureLabel);

	m_textureDock = new ads::CDockWidget(win_dockManager,
	                                     I18n::instance().translateCtx("Texture Viewer", "Dock"));
	// Not a UIPanel, so MakeDock() cannot supply the id — but the objectName is
	// still the key ADS serializes by, and the ctor derived it from translated
	// text. Set it explicitly, as a stable ASCII id, for the same reason.
	m_textureDock->setObjectName(QStringLiteral("dock.textureViewer"));
	m_textureDock->setWidget(textureWrap);
	m_textureDock->resize(300, 200);
	m_textureDock->setMinimumSize(200, 150);
	win_dockManager->addDockWidget(ads::BottomDockWidgetArea, m_textureDock);

	// --- Bottom: Log Panel (tabbed onto Texture Viewer area) ---
	auto logPanel = std::make_unique<neurus::LogPanel>();
	auto* logPanelRaw = logPanel.get();
	auto* logDock = MakeDock(logPanel.get());
	logDock->setWidget(logPanel.get(), ads::CDockWidget::ForceNoScrollArea);
	logDock->resize(640, 220);
	logDock->setMinimumSize(320, 140);
	win_dockManager->addDockWidget(ads::BottomDockWidgetArea, logDock,
	                               m_textureDock->dockAreaWidget());
	m_panels[PanelType::Log] = logPanel.release();
	m_docks[PanelType::Log] = logDock;

	// Error notifier: LogPanel signal -> status bar (3s, throttled by delta)
	QObject::connect(logPanelRaw, &neurus::LogPanel::errorNotified,
	                 this, [this](int delta, const QString& first) {
	                     statusBar()->showMessage(
	                         I18n::instance().translateCtx("%1 new error(s): %2", "StatusBar")
	                             .arg(delta).arg(first), 3000);
	                 });

	// Notify Application of the new native window handle for surface recreation
	UIEvents::instance().requestUIRecreation(reinterpret_cast<quintptr>(newHwnd));
}

// =========================================================================
// RetranslateAll — apply the active language to every user-visible string
// =========================================================================

void UIManager::RetranslateAll()
{
	auto& i18n = I18n::instance();

	// --- Menu bar ---
	for (const auto& [action, key] : m_menuItems)
		action->setText(i18n.translate(key));

	// --- Dock titles + per-panel texts ---
	for (const auto& [type, widget] : m_panels)
	{
		auto* panel = qobject_cast<UIPanel*>(widget);
		if (!panel)
			continue;
		auto dockIt = m_docks.find(type);
		if (dockIt != m_docks.end())
			dockIt->second->setWindowTitle(panel->PanelName());
		panel->Retranslate();
	}

	// --- Texture Viewer placeholder (not a UIPanel) ---
	if (m_textureDock)
		m_textureDock->setWindowTitle(i18n.translateCtx("Texture Viewer", "Dock"));
	if (m_textureLabel)
		m_textureLabel->setText(i18n.translateCtx("Texture Viewer", "Dock"));

	// --- Preferences dialog (if already opened) ---
	if (m_preferencesDialog)
		m_preferencesDialog->Retranslate();
}

// =========================================================================
// Preferences dialog — lazy creation + show
// =========================================================================

void UIManager::OpenPreferences()
{
	if (!m_preferencesDialog)
	{
		m_preferencesDialog = new PreferencesDialog(
			m_currentLanguage, m_targetFps, m_preferencesPath, this);

		// Dialog changes update the local cache and are routed through the
		// UIEvents bus so the Application applies + persists them
		// (UI → UIEvents → Application).
		connect(m_preferencesDialog, &PreferencesDialog::languageChangeRequested,
		        this, [this](const QString& lang) {
		            m_currentLanguage = lang;
		            neurus::UIEvents::instance().requestLanguageChange(lang);
		        });
		connect(m_preferencesDialog, &PreferencesDialog::targetFpsChangeRequested,
		        this, [this](int fps) {
		            m_targetFps = fps;
		            neurus::UIEvents::instance().requestTargetFps(fps);
		        });
	}

	// Re-seed the controls from the cache in case values changed since the
	// dialog was last shown (e.g. after a Reset to Defaults).
	m_preferencesDialog->SyncFrom(m_currentLanguage, m_targetFps);

	m_preferencesDialog->show();
	m_preferencesDialog->raise();
	m_preferencesDialog->activateWindow();
}

// =========================================================================
// Layout persistence
// =========================================================================

std::string UIManager::ExportLayout() const
{
	QByteArray geom  = saveGeometry();
	QByteArray state = win_dockManager->saveState();
	QByteArray packed = geom.toBase64() + '\n' + state.toBase64();
	return packed.toStdString();
}

void UIManager::ApplyLayout(const std::string& blob)
{
	if (blob.empty()) return;
	QByteArray packed = QByteArray::fromStdString(blob);
	int nl = packed.indexOf('\n');
	if (nl < 0)
	{
		NEURUS_ERR("[UIManager] Layout blob is malformed (no geometry/state "
		           "separator); keeping the default layout.");
		return;
	}
	QByteArray geom  = QByteArray::fromBase64(packed.left(nl));
	QByteArray state = QByteArray::fromBase64(packed.mid(nl + 1));
	if (!geom.isEmpty())  restoreGeometry(geom);
	if (!state.isEmpty())
	{
		// restoreState() only reports false for XML that fails to parse or has a
		// version mismatch. A blob whose dock names do not match ours parses fine
		// and returns true — ADS just skips the unknown names and leaves those
		// docks unassigned. So log the hard failure here, then repair either way.
		if (!win_dockManager->restoreState(state))
			NEURUS_ERR("[UIManager] Dock layout state could not be parsed; "
			           "falling back to default dock placement.");
		RepairOrphanedDocks();
	}
}

void UIManager::RepairOrphanedDocks()
{
	// flagAsUnassigned() clears the dock area, so a null area means the restored
	// state did not claim this dock: an unknown objectName (a foreign or stale
	// blob) or a panel added since the project was saved.
	//
	// Re-dock the existing widgets rather than calling RestoreDefaultLayout():
	// that re-runs CreateDocks(), which would destroy the Viewport and hand out a
	// new native window handle. ApplyLayout() runs before the Application wires
	// its signals, so nothing would be listening to re-create the VkSurfaceKHR
	// already built from the old handle.
	QStringList repaired;
	auto repair = [&](ads::CDockWidget* dock, ads::DockWidgetArea area) {
		if (!dock || dock->dockAreaWidget()) return;
		win_dockManager->addDockWidget(area, dock);
		dock->toggleView(true);
		repaired << dock->objectName();
	};

	for (const auto& [type, dock] : m_docks)
		repair(dock, DefaultDockArea(type));
	repair(m_textureDock, ads::BottomDockWidgetArea);

	if (!repaired.isEmpty())
		NEURUS_ERR("[UIManager] Saved layout did not claim these docks; restored "
		           "them to default areas: "
		           << repaired.join(QStringLiteral(", ")).toStdString());
}

void UIManager::RestoreDefaultLayout()
{
	// Clear panel/dock maps before dock manager destroys its children.
	// Dock widgets and their contained panels are owned by CDockManager
	// via Qt parent-child; clearing the maps just drops our non-owning pointers.
	m_panels.clear();
	m_docks.clear();

	auto docks = win_dockManager->dockWidgetsMap();
	for (auto it = docks.begin(); it != docks.end(); ++it)
	{
		it.value()->deleteDockWidget();
	}

	// Re-create the default dock arrangement (emits uiRecreated)
	CreateDocks();
}

} // namespace neurus
