# UI Layer

The Neurus UI layer is a Qt6 Widgets application built on Qt-Advanced-Docking-System (ADS) that presents editor state, hosts the Vulkan viewport, and forwards user intent to lower layers as Qt signals.

## Architecture

The UI is the top of the unidirectional isolation chain:

```
UI Layer (Qt6 Widgets + ADS)
  owns: the QMainWindow, the Viewport window, UIEvents (QObject singleton)
  QML provides window + input ONLY. No rendering logic.
        |
        | Qt signals / slots
        v
Editor Layer
  owns: Scene, RenderConfig, UploadManager, Controllers
        |
        v
Renderer Layer (Vulkan-HPP vk::raii)
  consumes: the viewport's native window handle (read-only)
```

- UI emits signals via `UIEvents`. It never calls the Editor or Renderer directly.
- The Editor translates raw input into typed events through `EventQueue` and dispatches them to Controllers.
- The Renderer receives read-only scene data through `Context` objects and never calls back into the Editor.

Cross-layer communication must go through `UIEvents` (UI to Editor) or `Context` objects; inside the Editor, controllers communicate through the typed `EventQueue`, which is not a channel to the Renderer. Direct coupling across layers is forbidden - the Renderer must not include Editor or UI headers, and the UI must not include Renderer headers (it exposes only the viewport's native window handle).

The UI layer owns the main window, the dock layout, and the viewport widget. The viewport exposes a native window handle (`Viewport::hwnd()`); the surface and every other Vulkan object live below the UI.

## UIManager

`UIManager` (`src/ui/UIManager.h/cpp`) is the `QMainWindow` subclass at the center of the layer. It owns:

- the ADS dock manager (`ads::CDockManager`),
- the menu bar and its translation registry,
- the panel registry (`std::map<PanelType, QWidget*>` plus `std::map<PanelType, ads::CDockWidget*>`),
- the per-frame `Refresh()` pipeline,
- layout export/apply, retranslation fan-out, and the Preferences dialog.

The Application seeds it with plain values (`UIManager(language, targetFps, prefsPath)`) - the UI never touches the app-layer `Preferences` type.

`Refresh(const UIContext& ctx)` iterates every registered panel and calls `Refresh(ctx)` on each; it also caches the undo/redo stack labels for the Edit submenus. It is called from the frame loop after the renderer draws, keeping widgets in sync with Editor/Project state.

`GetPanel<T>()` returns a typed pointer to a panel by `PanelClass::kType` (each panel class provides `static constexpr PanelType kType`), and `GetDock(PanelType)` returns the dock widget.

## Dock layout (ADS)

Panels are created in `CreateDocks()` and placed in dock areas:

```
Left:   Viewport | Shader Editor | Outliner
Right:  Property Panel | Render Config
Bottom: Profiling | Texture Viewer | Log
```

- The Viewport is created **first** (an ADS requirement) and added to the left area, with closable disabled.
- The Texture Viewer is a non-`UIPanel` placeholder dock (a centered `QLabel`); the Log panel is tabbed onto its area.
- Panels use `ads::CDockWidget::ForceNoScrollArea` where a scroll area would fight the hosted content.

Docks that host a `UIPanel` are created through `UIManager::MakeDock()`, which titles them with the translated `PanelName()` and then overwrites `objectName` with the stable `PanelId()`. Non-`UIPanel` docks (the Texture Viewer) set their own `dock.*` objectName explicitly.

## Viewport embedding

The `Viewport` is a `QWidget` that sets `WA_NativeWindow` and `WA_OpaquePaintEvent`, overrides `paintEvent()` to a no-op, and forces native window creation with `winId()`. It owns no Vulkan resources: it provides the native window handle only, and emits `resized(int, int)` so the renderer can recreate the swapchain. `UIManager::getViewportHwnd()` / `getViewportWidth()` / `getViewportHeight()` expose it to the Application.

`CreateDocks()` requests surface recreation through `UIEvents::requestUIRecreation()` with the new native handle.

## Layout persistence

Dock layout is **project state**, not app state: `UIManager::ExportLayout()` / `ApplyLayout()` produce and consume one opaque blob (base64 window geometry + `\n` + base64 ADS dock state), and `asset/components/UIComponent` carries it in the `.neurus.json` project file. The Application owns persistence; the UI layer never touches a path. **View -> Restore Default Layout** re-runs `CreateDocks()`.

**The blob restores window geometry, never the full-screen state.** `saveGeometry()` carries the window *state* alongside the rectangle, and `restoreGeometry()` re-applies it, so a blob saved while the window was full screen started the app full screen. A full-screen `QMainWindow` has no frame - no minimize, maximize or close button, and nothing in the menus to undo it - so the only ways out were `Alt+F4` and File -> Exit. Measured on Windows, the restored window had `WS_CAPTION` / `WS_MINIMIZEBOX` / `WS_MAXIMIZEBOX` / `WS_THICKFRAME` all clear at exactly the monitor size, while the same window with an emptied blob had all four set at 1600x900. `ApplyLayout()` therefore clears a restored full-screen flag (`setWindowState()` rather than `showNormal()`, which would *show* the window from there, before the Application does), and full screen becomes a deliberate state instead - **View -> Full Screen**, F11 via `QKeySequence::FullScreen`. `changeEvent()` keeps that checkable action in step, so the check mark follows the state however it was entered; `setChecked()` with an unchanged value emits nothing, so the two cannot bounce.

**Identity vs display - the rule that keeps layouts portable.** ADS serializes dock state keyed by `objectName`, and `ads::CDockWidget`'s constructor copies the ctor *title* into `objectName`. Once dock titles became translatable, that made the serialization key language-dependent: a layout saved in one language restored to a blank window in another, because every lookup missed. So:

> Display text is translated. Identity keys never are.

- `UIPanel::PanelIdFor(PanelType)` returns a stable ASCII id (`"dock.viewport"`, `"dock.outliner"`, ...). It is independent of the display string as well as of the language - reusing the English msgid would break every saved layout on a UI-string rename.
- Every panel dock MUST be created through `UIManager::MakeDock()`, which titles it with `PanelName()` (translated) and then overwrites `objectName` with `PanelId()`. Centralising this is the point: an omitted `setObjectName` at one of eight call sites is invisible until someone switches language and relaunches.
- Non-`UIPanel` docks (the Texture Viewer placeholder) set their own `dock.*` objectName explicitly at the creation site.
- `RetranslateAll()` needs no special handling: `setWindowTitle()` does not touch `objectName`, which is exactly the desired split.

**Degradation.** ADS silently skips objectNames it does not recognise and returns `true`, so `restoreState()`'s result cannot detect a foreign or stale blob - only unclaimed docks can. `ApplyLayout()` therefore logs the bool (real XML parse failures) and always follows with `RepairOrphanedDocks()`, which re-docks any dock whose `dockAreaWidget()` is null into a default area. It re-docks the **existing** widgets rather than calling `RestoreDefaultLayout()`: that would re-run `CreateDocks()` and hand out a new Viewport window handle, and `ApplyLayout()` runs before the Application wires its signals, so nothing would rebuild the surface already created from the old handle.

**The checked-in default project carries a layout blob too.** `res/shadow.neurus.json` (loaded at startup by the Application) stores the ADS state under `project.ui` as `geometry_base64 + "\n" + state_base64`, each half a `qCompress` blob. It is a build artifact of whoever last saved it, so it must be verified rather than trusted - a copy saved before the `PanelId` split contained nothing but non-English dock names, which meant an English machine started with an empty window. Decode and check it externally:

```python
import base64, json, re, zlib
blob = json.load(open("res/shadow.neurus.json"))["project"]["ui"]
xml = zlib.decompress(base64.b64decode(blob.split("\n")[1])[4:]).decode()
print(re.findall(r'Name="([^"]+)"', xml))   # every entry must be a dock.* id
```

The app loads and writes back the *build* copy (`build/debug/res/shadow.neurus.json`, refreshed from `res/` by a `copy_directory` step), so re-saving the default layout means copying the result back into `res/` by hand.

## Dock features

- Viewport: closable disabled; the other docks keep closable + movable + floatable.
- Config flags: `OpaqueSplitterResize=false` (better native-window container behaviour), `FocusHighlighting=true`.

**Full screen forbids tearing panels off.** A torn-off panel is a separate top-level window, and a separate window cannot be used inside another window's full-screen space: macOS gives ADS's plain `Qt::Window` floating container `NSWindowCollectionBehaviorFullScreenPrimary` and the window server then refuses to let the user move it - the panel appears and is frozen. Full screen is entered from **View -> Full Screen** (F11) or the OS's native full-screen control, so the source of the state is not always ours. `UIManager::changeEvent()` watches `QEvent::WindowStateChange` (the only signal Qt gives for a native full-screen transition) and calls `SetFloatingLocked()` while full screen, after docking any already-floating panel back via `DockFloatingPanels()`.

Only `Floatable` is locked, so dragging a panel to a different *dock position* still works - only the "release outside any dock area" gesture snaps back, because `FloatingDragPreview::createFloatingWidget()` finds no floatable content. The lock is a mask over `CDockWidget::features()`, not a rewrite of the per-widget flags, so leaving full screen restores whatever each panel had. ADS's `notifyFeaturesChanged()` also greys out the undock button and the Detach menu entry, making the restriction visible.

## Menu bar

| Menu | Items |
|------|-------|
| **File** | New, Open..., Save, Save As..., Preferences... (`Ctrl+,`), Exit |
| **View** | Restore Default Layout, Full Screen (`F11`) |
| **Edit** | Undo, Redo, Add (Mesh... / Camera / Light > Point, Sun, Spot) |
| **Tools** | Take Screenshot (`F12`), Screenshot All Passes (`Ctrl+F12`) |
| **Help** | About Neurus |

Each menu action is registered as an `(QAction*, key)` pair so `RetranslateAll()` can re-text it on a language change.

## Panel system

All dock panels inherit from `UIPanel` (`src/ui/panels/UIPanel.h`):

- `PanelType` enum identifies each panel (`Viewport`, `Outliner`, `PropertyPanel`, `RenderConfig`, `ShaderEditor`, `Profiling`, `Log`).
- `virtual void Refresh(const UIContext& ctx)` - called per-frame to sync panel UI with application state.
- `PanelName()` returns the translated dock title; `PanelId()` returns the stable serialization key.
- `virtual void Retranslate()` re-applies user-visible text on a language change (no-op by default).

See [Panels](panels.md) for each panel, and [Items](items.md), [Models](models.md) and [i18n](i18n.md) for the shared building blocks.

`UIContext` is the UI's view of the frame, two fields wide: `scene` (`const Scene*`) and `config` (`const RenderConfig*`), both filled by `Editor::UpdateUIContext()` and both plain forward-declared pointers. It deliberately carries no camera and no overlay draw list - a panel has no business naming either - so the UI's view is a property of the type rather than a convention. The render settings are read through `Settings()`, which never returns null: it falls back to `core/RenderConfig.h`'s `DefaultRenderConfig()`, the same single fallback `RenderContext::Settings()` applies, so a panel and a pass cannot disagree about an unset value. `scene` must still be checked - `HasScene()` - before dereferencing.

`UIContext` also carries four typed pointers, each forward-declared in `UIContext.h`, as the Application's half: `const FrameProfile* profile` (the profile returned by `DeferredRenderer::DrawFrame()`), `const LogBuffer* log`, `const HistoryView* history` and `const ScriptConsoleBuffer* scriptConsole` (the script engine's console buffer). The Editor fills `scene` and `config` through `Editor::UpdateUIContext()`; the Application adds these four each frame, and the `ProfilingPanel` reads `profile` directly to render a real-time per-pass tree. All four are null in a default-constructed `UIContext` - what a widget test builds - so a panel checks before dereferencing.

## Refresh pipeline

1. The Editor fills the document half of a `UIContext` (`scene`, `config`) through `Editor::UpdateUIContext()`; the Application adds the frame-scoped half (profile, log, history, script console).
2. `UIManager::Refresh(ctx)` runs after the renderer draws, caching undo/redo labels and iterating the panel registry.
3. Each `UIPanel::Refresh(ctx)` extracts the data it needs (RenderConfig, Scene, profile, log) and updates its widgets.

Widgets must dirty-check before applying a value, so a per-frame push never fights the control the user is editing. See the [Items](items.md) and [Models](models.md) conventions.

## Preferences

App-level settings (distinct from the per-project `.neurus.json` files) live in `~/.neurus/preferences.json`, persisted with cereal JSON by the `Preferences` struct in `src/app/Preferences.h/cpp` - an **Application Layer** concept (future settings: CUDA enablement, theme, shortcut schemes).

- Fields: `language` (`"en"` / `"zh_CN"`, or `"auto"` = follow the system UI language) and `target_fps` (0 = unlimited).
- **`"auto"` is stored verbatim and stays that way.** `I18n::setLanguage()` resolves the sentinel itself, so nothing on the load path rewrites the preference with a concrete code - resolving it in place would turn "follow the system language" into "pin to whatever the system said at first launch" the moment the file was next written.
- The **Application is the sole manager**: it loads the file before the window is built, applies the saved language and target FPS, saves on every edit, and saves once more on `aboutToQuit`.
- **Loading distinguishes missing from corrupt** (`Preferences::LoadResult`), and never mutates the fields on either failure - parsing goes into a scratch copy, so a truncated or schema-drifted file cannot leave values half-applied:
  - `Ok` - values in effect.
  - `Missing` - first run; the Application writes the defaults.
  - `Corrupt` - the Application logs it, keeps defaults, and calls `Preferences::Backup()` to move the file to `preferences.json.bak`. It must **not** save instead: that would destroy still-recoverable settings, and the move also stops the `aboutToQuit` save from clobbering the evidence.
- **The UI never touches the `Preferences` type.** The Application seeds the window with plain values; the `PreferencesDialog` is a pure UI widget holding only those plain values and emits requests through `UIEvents`. See [Panels](panels.md) for the dialog.

## Patterns

### Loading QSS stylesheets from Qt resources

Stylesheets are stored as `.qss` files in `src/ui/qml/` and embedded via `qt_add_resources`, same as QML. Use `setObjectName()` plus QSS ID selectors instead of per-widget `setStyleSheet()` calls:

```
// 1. Embed in CMakeLists.txt
qt_add_resources(neurus_ui "resources" PREFIX "/" FILES qml/outliner.qss)

// 2. Load once (static, one-shot)
QFile file(":/qml/outliner.qss");
file.open(QIODevice::ReadOnly | QIODevice::Text);
QString stylesheet = QTextStream(&file).readAll();

// 3. Apply to parent widget - ID selectors cascade to children
parentWidget->setStyleSheet(stylesheet);

// 4. In C++, set object names matching QSS ID selectors
childBtn->setObjectName("myButton");  // matches QPushButton#myButton

// 5. Per-instance overrides via setStyleSheet() on the specific child
childBtn->setStyleSheet("QPushButton { color: #ff6f00; }");
```

`src/ui/qml/outliner.qss` is the canonical example, using `QPushButton#outlinerNameBtn` and `QPushButton#outlinerToggleBtn` selectors.

### Lazy updates via logical state tracking

Avoid redundant widget operations by storing the last-known state and skipping updates when nothing changed:

```cpp
// DO: dirty-check before applying
if (m_eyeVisible != viewportVisible)
{
    m_eyeBtn->blockSignals(true);
    m_eyeBtn->setChecked(viewportVisible);
    m_eyeBtn->blockSignals(false);
    m_eyeVisible = viewportVisible;
    setEyeBtnColor();
}

// DON'T: always apply, even when identical
m_eyeBtn->setChecked(viewportVisible);
setEyeBtnColor();
```

This applies to any `Refresh()`-based panel - cache the previous value of each widget-modifying input and gate the write behind an equality check.

### Prefer QSS over inline stylesheets

Setting `setStyleSheet()` on individual widgets forces Qt to re-parse the CSS text and re-resolve all selectors, which is measurably slower than QSS class selectors with dynamic properties:

```cpp
// DO: QSS with dynamic property (fast, declarative)
//  .qss: QPushButton#outlinerNameBtn[selectionState="active"] { color: #ff6f00; }
m_nameBtn->setProperty("selectionState", "active");
m_nameBtn->style()->unpolish(m_nameBtn);
m_nameBtn->style()->polish(m_nameBtn);

// DON'T: inline stylesheet (slow, re-parses CSS text each time)
m_nameBtn->setStyleSheet("QPushButton { color: #ff6f00; }");
```

The same principle applies to `setIcon()` on toggle buttons - use `GetIconPair()` to pre-bake On/Off states into a single `QIcon` instead of string-building icon names and calling `GetIcon()` on every toggle.

### Naming convention: `set*` for Refresh-path methods

Widget methods called from `Refresh()` or other per-frame update paths must use Qt-style lowercase naming (e.g. `setObject()`, `setValue()`, `setRowIndex()`) rather than PascalCase. This aligns with Qt's own convention for setter methods (`QSpinBox::setValue()`, `QWidget::setVisible()`) and distinguishes them from application-level PascalCase methods (`BuildTransformEditor()`, `PopulateTransform()`).

New reusable widgets in `src/ui/items/` (and models/delegates/utils) should follow this convention for all public setters that participate in the Refresh pipeline.

## Layer rules

The UI MAY:

- own the `QMainWindow` and the Viewport window;
- emit `UIEvents` signals;
- handle Qt events (resize, close, menu actions);
- manage the ADS dock layout.

The UI MUST NOT:

- directly call Renderer methods (go through `UIEvents`);
- create Vulkan objects beyond the viewport's native window handle;
- mutate scene state directly;
- access GPU resources directly;
- redefine the UI elements that are already in `src/ui/items/`.

## Build integration

```cmake
# ADS submodule (static build)
add_subdirectory(lib/src/qtadvanceddocking)

# Link targets
target_link_libraries(neurus_ui PUBLIC
    Qt6::Core Qt6::Widgets
    neurus_editor
    ads::qtadvanceddocking-qt6)
target_compile_definitions(neurus_ui PUBLIC ADS_STATIC)
```

`src/ui/CMakeLists.txt` embeds QSS/QML under `/`, SVG icons under `/icons` (files in `res/ui/icons/`), and globs `res/i18n/*.po` into `qt_add_resources` with `CONFIGURE_DEPENDS`.
