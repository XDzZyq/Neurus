# UI System

## Overview

The UI layer is a **Qt6 Widgets** application with **Qt-Advanced-Docking-System (ADS)** for dockable panels and **QVulkanWindow** for GPU rendering. It communicates with other layers exclusively through the EventBus (Qt Signals/Slots).

## Location

| File | Purpose |
|------|---------|
| `src/ui/UIManager.h/cpp` | QMainWindow subclass with ADS dock manager + menus |
| `src/ui/UIManager.h/cpp` | Per-frame `Refresh()` pipeline, panel registry (`GetPanel<T>`) |
| `src/ui/UIContext.h/cpp` | Per-frame UI data snapshot (carries RenderConfig pointer) |
| `src/ui/panels/UIPanel.h` | Base class for all dock panels with `PanelType` enum |
| `src/ui/panels/Viewport.h/cpp` | Native HWND Vulkan surface widget (Viewport dock) |
| `src/ui/panels/Outliner.h/cpp` | Scene object hierarchy tree (Outliner dock) |
| `src/ui/panels/PropertyPanel.h/cpp` | Object property inspector with GOType-aware subpanels (Property dock) |
| `src/ui/panels/RenderConfigPanel.h/cpp` | Live render config controls (Render Config dock) |
| `src/ui/panels/ProfilingPanel.h/cpp` | Real-time GPU profiling tree (Profiling dock) |
| `src/ui/panels/ShaderEditorPanel.h/cpp` | Shader editor dock — Code mode + Structure mode (tree-based Struct Editor) |
| `src/ui/panels/LogPanel.h/cpp` | Realtime log viewer dock: filter bar + list view (issue #39) |
| `src/ui/panels/PreferencesDialog.h/cpp` | Preferences dialog — live-applied language + target FPS (File → Preferences…, Ctrl+,) |
| `src/ui/utils/I18n.h/cpp` | Lightweight runtime i18n manager: dictionary-based, instant language switch |
| `src/ui/utils/POCatalog.h/cpp` | Standalone gettext `.po` parser (`neurus::po`) behind `I18n` — no Qt widgets, unit-testable |
| `src/ui/models/ShaderStructModel.h/cpp` | QAbstractItemModel tree for the ShaderStruct IR (3-level: sections → fields/structs → members) |
| `src/ui/models/LogModel.h/cpp` | QAbstractListModel over the core LogBuffer |
| `src/ui/models/LogFilterProxy.h/cpp` | QSortFilterProxyModel: level filter + text search |
| `src/ui/delegates/ShaderFieldDelegate.h/cpp` | Delegate: QComboBox (type) / QLineEdit (name) editors + painted "+" glyph for section/struct-def rows |
| `src/ui/delegates/LogDelegate.h/cpp` | Severity-colored row delegate for the log view |
| `src/ui/items/CodeEditor.h/cpp` | GLSL code editor widget (line numbers, monospace font) |
| `src/ui/utils/ShaderHighlighter.h/cpp` | GLSL syntax highlighter for the code editor |
| `src/ui/presets/CameraProperties.h/cpp` | Camera property editor preset (target Vec3Spin + FOV ScalarSlider) |
| `src/ui/presets/MeshProperties.h/cpp` | Mesh property editor preset (path label + shadow/material checkboxes) |
| `src/ui/presets/LightProperties.h/cpp` | Light property editor preset (type label + power/radius sliders + shadow checkbox) |
| `src/ui/presets/EnvironmentProperties.h/cpp` | Environment property editor preset (path label + intensity/rotation sliders) |
| `src/ui/presets/DebugProperties.h/cpp` | Debug object property editor preset — one preset for DebugLine/DebugPoints/DebugMesh (issue #22) |
| `src/ui/Icons.h/cpp` | Static SVG icon library with lazy-loaded QIcon cache |
| `src/ui/items/ScalarSlider.h/cpp` | Reusable slider+spinbox composite widget |
| `src/ui/items/Vec3Spin.h/cpp` | Reusable XYZ triple-spinbox composite widget |
| `src/ui/items/ColorButton.h/cpp` | Color swatch button opening QColorDialog; optional alpha channel |
| `src/ui/items/OutlinerRow.h/cpp` | Pool-recyclable outliner row with type icon, name, toggles |
| `src/ui/qml/` | Qt resource files: QML layouts, QSS stylesheets (embedded at build time) |
| `src/ui/VulkanWindow.h/cpp` | QVulkanWindow subclass hosting the triangle renderer |

## Rendering Architecture

### Primary Path: QVulkanWindow (C Vulkan API)

```
QVulkanInstance
  └── VulkanWindow : QVulkanWindow
        ├── Qt handles: instance, surface, device, swapchain, command buffer
        └── QVulkanRenderer : QVulkanWindowRenderer
              ├── initResources() → create pipeline + shader modules
              ├── startNextFrame() → vkCmdBeginRenderPass → draw(3) → endRenderPass
              │   → frameReady() → requestUpdate()
              └── releaseResources() → destroy pipeline, shader modules
```

- `QVulkanWindow` handles ALL infrastructure: instance, surface, device, swapchain (including recreation), command pool, primary command buffer
- `QVulkanRenderer` implements `QVulkanWindowRenderer` with C Vulkan API
- Pipeline uses `QVulkanWindow::defaultRenderPass()` (traditional render pass, not dynamic rendering)
- Continuous rendering via `requestUpdate()` in `startNextFrame()`

### Secondary Path: vk::raii + Dynamic Rendering (reference implementation)

```
VulkanWidget (QWidget with WA_NativeWindow)
  → vk::Win32SurfaceCreateInfoKHR → vk::raii::SurfaceKHR
  → VulkanContext → vk::raii::Device → Renderer → Swapchain → Shader
```

- `VulkanWidget` provides native HWND for Vulkan surface
- `Renderer::DrawFrame()` uses vk::raii fences/semaphores with `VK_KHR_dynamic_rendering`
- QTimer at ~60 FPS drives the render loop on the main thread
- **Re-entrancy guard** (`static bool s_frameInProgress`) prevents nested `DrawFrame()` calls
- **Generation counter** on `Swapchain` ensures command buffers are re-recorded after swapchain recreation

## Dock Layout (ADS)

```
ads::CDockManager
├── CenterDockWidgetArea: Viewport (QVulkanWindow via QWidget::createWindowContainer)
├── LeftDockWidgetArea:   Shader Editor
├── RightDockWidgetArea:  Outliner | Property Editor | Render Config (tabbed)
└── BottomDockWidgetArea: Profiling | Texture Viewer (tabbed)
```

### Viewport Embedding

The `QVulkanWindow` is embedded into the dock widget via `QWidget::createWindowContainer()`:

```cpp
auto* vulkanWindow = new VulkanWindow(&qVkInstance, vertSpv, vertSize, fragSpv, fragSize);
QWidget* container = QWidget::createWindowContainer(vulkanWindow);
mainWindow->createViewportDock(container);
```

### Layout Persistence

Dock layout is **project state**, not app state: `UIManager::ExportLayout()` /
`ApplyLayout()` produce and consume one opaque blob (base64 window geometry +
`\n` + base64 ADS dock state), and `asset/components/UIComponent` carries it in
the `.neurus.json` project file. The Application owns persistence; the UI layer
never touches a path. **View → Restore Default Layout** re-runs `CreateDocks()`.

**The blob restores window geometry, never the full-screen state.**
`saveGeometry()` carries the window *state* alongside the rectangle, and
`restoreGeometry()` re-applies it, so a blob saved while the window was full
screen started the app full screen. A full-screen `QMainWindow` has no frame:
no minimize, maximize or close button, and nothing in the menus to undo it, so
the only ways out were `Alt+F4` and File → Exit. Measured on Windows — the
restored window had `WS_CAPTION`/`WS_MINIMIZEBOX`/`WS_MAXIMIZEBOX`/
`WS_THICKFRAME` all clear at exactly the monitor size, while the same window
with an emptied blob had all four set at 1600x900. `ApplyLayout()` therefore
clears a restored full-screen flag (`setWindowState()` rather than
`showNormal()`, which would *show* the window from there, before `Application`
does), and full screen becomes a deliberate state instead — **View → Full
Screen**, F11 via `QKeySequence::FullScreen`. `changeEvent()` keeps that
checkable action in step, so the check mark follows the state however it was
entered; `setChecked()` with an unchanged value emits nothing, so the two cannot
bounce.

**Identity vs display — the rule that keeps layouts portable.** ADS serializes
dock state keyed by `objectName`, and `ads::CDockWidget`'s constructor copies the
ctor *title* into `objectName` (`DockWidget.cpp:385-386`). Once dock titles became
translatable, that made the serialization key language-dependent: a layout saved
in one language restored to a blank window in another, because every lookup
missed. So:

> **Display text is translated. Identity keys never are.**

- `UIPanel::PanelIdFor(PanelType)` returns a stable ASCII id (`"dock.viewport"`,
  `"dock.outliner"`, …). It is independent of the display string as well as of
  the language — reusing the English msgid would break every saved layout on a
  UI-string rename.
- Every panel dock MUST be created through `UIManager::MakeDock()`, which titles
  it with `PanelName()` (translated) and then overwrites `objectName` with
  `PanelId()`. Centralising this is the point: an omitted `setObjectName` at one
  of eight call sites is invisible until someone switches language and relaunches.
- Non-`UIPanel` docks (the Texture Viewer placeholder) set their own
  `dock.*` objectName explicitly at the creation site.
- `RetranslateAll()` needs no special handling: `setWindowTitle()` does not touch
  `objectName`, which is exactly the desired split.

**Degradation.** ADS silently `continue`s past objectNames it does not recognise
and returns `true`, so `restoreState()`'s result cannot detect a foreign or stale
blob — only unclaimed docks can. `ApplyLayout()` therefore logs the bool (real
XML parse failures) and always follows with `RepairOrphanedDocks()`, which
re-docks any dock whose `dockAreaWidget()` is null into a default area. It
re-docks the **existing** widgets rather than calling `RestoreDefaultLayout()`:
that would re-run `CreateDocks()` and hand out a new Viewport window handle, and
`ApplyLayout()` runs before the Application wires its signals, so nothing would
rebuild the `VkSurfaceKHR` already created from the old handle.

Regression coverage: `test/ui/test_dock_identity.cpp` (ids are stable ASCII and
language-independent; layouts round-trip in both language directions; a real
pre-fix Chinese-keyed blob and a malformed blob both degrade to a populated
layout, not a blank window).

**The checked-in default project carries a layout blob too.**
`res/shadow.neurus.json` (loaded at startup by `Application`) stores the ADS
state under `project.ui` as `geometry_base64 + "\n" + state_base64`, each half a
`qCompress` blob. It is a build artifact of whoever last saved it, so it must be
verified rather than trusted — a copy saved before the `PanelId` split contained
nothing but Chinese dock names, which meant an English machine started with an
empty window. Decode and check it externally:

```python
import base64, json, re, zlib
blob = json.load(open("res/shadow.neurus.json"))["project"]["ui"]
xml = zlib.decompress(base64.b64decode(blob.split("\n")[1])[4:]).decode()
print(re.findall(r'Name="([^"]+)"', xml))   # every entry must be a dock.* id
```

Note the app loads and writes back the *build* copy
(`build/debug/res/shadow.neurus.json`, refreshed from `res/` by a
`copy_directory` step), so re-saving the default layout means copying the result
back into `res/` by hand.

### Dock Features

- Viewport: ADS central widget — closable/movable/floatable all disabled by ADS.
- All other docks: closable + movable + floatable.
- Config flags: `OpaqueSplitterResize=false` (better Vulkan container behavior),
  `FocusHighlighting=true`.

**Full screen forbids tearing panels off.** A torn-off panel is a separate
top-level window, and a separate window cannot be used inside another window's
full-screen Space: macOS gives ADS's plain `Qt::Window` floating container
`NSWindowCollectionBehaviorFullScreenPrimary` (`qcocoawindow.mm`,
`setWindowFlags`) and the window server then refuses to let the user move it —
the panel appears and is frozen. Full screen is entered from **View → Full
Screen** (F11) or macOS's native green button — the latter is why the source of
the state is not always ours. `UIManager::changeEvent()` watches
`QEvent::WindowStateChange` (the only signal Qt gives for macOS's native green
button) and calls `lockDockWidgetFeaturesGlobally(DockWidgetFloatable)` while
full screen, after docking any already-floating panel back via
`DockFloatingPanels()`.

Only `Floatable` is locked, so dragging a panel to a different *dock position*
still works — only the "release outside any dock area" gesture snaps back,
because `FloatingDragPreview::createFloatingWidget()` finds no floatable content.
The lock is a mask over `CDockWidget::features()`, not a rewrite of the
per-widget flags, so leaving full screen restores whatever each panel had. ADS's
`notifyFeaturesChanged()` also greys out the undock button and the Detach menu
entry, making the restriction visible. Covered by
`test/ui/test_floating_window.cpp`.

## Menu Bar

| Menu | Items |
|------|-------|
| **File** | New, Open…, Save, Save As…, Preferences… (`Ctrl+,`), Exit (`Alt+F4`) |
| **View** | Restore Default Layout, Full Screen (`F11`) |
| **Edit** | Undo, Redo, Add (Mesh… / Camera / Light) |
| **Tools** | Take Screenshot (`F12`), Screenshot All Passes (`Ctrl+F12`) |
| **Help** | About Neurus |

## Internationalization (i18n)

Language switching is **live** — no restart, no Qt Linguist toolchain — and
the catalogs use the **GNU gettext .po format** (the same system Blender
uses), so translators can work with Poedit / Weblate. The `I18n` singleton
(`src/ui/utils/I18n.h/cpp`) owns the active language code and a
`QHash<(context, msgid), QString>` catalog; the **file format** half lives in
`src/ui/utils/POCatalog.h/cpp` (`neurus::po::Parse` / `po::HeaderField`), split
out so it is unit-testable without a singleton or a Qt resource
(`test/ui/test_po_catalog.cpp`):

- **msgid keys are the English display strings themselves.** A missing
  catalog, context or msgid makes `I18n::translate*()` return the key
  verbatim, so English is the implicit built-in fallback.
- Per-language catalogs are embedded as Qt resources
  (`:/i18n/<code>.po`, see `res/i18n/zh_CN.po`), parsed on demand by
  `setLanguage()`. Values may contain `%1/%2` placeholders (callers apply
  `.arg()`).
- **Contexts (msgctxt) disambiguate identical English strings:**
  `translate(key)` uses the default context; `translateCtx(key, "Dock" |
  "Tooltip" | "Dialog" | "StatusBar" | "Placeholder")` targets a specific
  one. Keys that are forwarded to helpers instead of written as
  `translate("...")` literals are marked with the no-op `N_()` macro
  (gettext convention) so the extractor can find them (menus,
  RenderConfigPanel helper args).
- `setLanguage()` emits `languageChanged()` only when the code actually
  changes. `I18n::supportedLanguages()` lists shipped languages (native
  display names); `I18n::systemLanguage()` detects the OS UI language
  (Simplified-Chinese → `zh_CN`, else `en`).
- **The parser accepts gettext, not just our own output.** Entries end at the
  next `msgctxt`, at the next `msgid` that does not follow a `msgctxt`, or at a
  blank line — blank separators are conventional, not required, and treating
  them as the only terminator made every key inherit the previous entry's
  `msgstr` (with the metadata header at the top of every file, the first real
  key resolved to the header text). Plural forms are **dropped, not merged**:
  `msgid_plural` must be tested *before* `msgid`, of which it is a prefix, and
  only `msgstr[0]` is kept. Both shapes are what hand-editing and Poedit
  produce, which is the whole reason the `.po` format was chosen.

**How retranslation reaches the widgets:**

1. `UIPanel` stores its dock title as a translation *key* (`nameKey`, default
   derived from `PanelType`); `PanelName()` resolves it through `I18n` in the
   `"Dock"` context at call time, so dock titles follow the language with no
   per-panel code. The dock's `objectName` is a **separate, never-translated**
   id (`PanelIdFor()`) — see *Layout Persistence* for why.
2. Every panel may override `UIPanel::Retranslate()` to re-apply its own
   labels/buttons/combo items. Panels call `Retranslate()` once at the end of
   their constructor (so the startup language applies) and the framework calls
   it again on every language change.
3. `UIManager::RetranslateAll()` (connected to `I18n::languageChanged()`) is
   the single fan-out: it re-texts every menu action from its registered
   `(QAction*, key)` pair, updates every dock title via `setWindowTitle()`
   (ADS propagates to the tab), calls each panel's `Retranslate()`, and
   re-translates the Texture Viewer placeholder + Preferences dialog.
4. The Preferences dialog also connects to `languageChanged()` directly so it
   retranslates while open.

**Two rules that make retranslation order-independent.** `Retranslate()` runs
once from the constructor and again on every switch, so a fix that only works in
one of those two orders is not a fix:

- **If a code path rewrites a string later, cache the translation — do not
  re-stamp a literal.** `ProfilingHead` writes column 0 on every
  `NoData ↔ Frame` transition, so it stores `m_frameLabel` / `m_noDataLabel`
  (seeded by `retranslate()`) and writes those; a `QStringLiteral("Frame")`
  there re-Englished the row on the next transition after a switch. Same reason
  `Outliner::EnsureRowPool()` calls `RetranslateRow()` on each new row: rows
  created *after* a switch would otherwise keep the constructor's English
  tooltips, and `Refresh()` never rewrites them.
- **A cached data key is empty before anything is bound, and `translate("")`
  returns `""`.** `LightProperties::Retranslate()` resolves the light-type name
  from `m_cachedType`, which is empty at constructor time and again after
  `setObjectId()`, so it falls back to `translate("Unknown")` instead of
  blanking the row.

**Item models retranslate too.** `QAbstractItemModel` subclasses that produce
display text (`ShaderStructModel::headerData()`, `sectionTitle()`) call `I18n`
**on demand** and expose a `Retranslate()` that only re-emits
(`headerDataChanged` + `dataChanged` over the section rows) — no rebuild, since
nothing is cached. Section nodes store their `ShaderSection`, never a title
string. The owning panel calls it from its own `Retranslate()`
(`ShaderEditorPanel`). `QStyledItemDelegate` editors are created on demand, so
they translate at `createEditor()` time and need no hook at all
(`ShaderFieldDelegate`'s `"struct name"` placeholder).

**Dialog filters are display text.** `QFileDialog` name filters go through
`translateCtx(..., "Dialog")` like the dialog titles — `"Neurus Project
(*.neurus.json)"`, `"OBJ Files (*.obj)"`, `"Log Files (*.log)"`.

**Translation management (Blender-style pipeline):**

- `scripts/extract_i18n.py` scans `src/ui/` for `translate()` /
  `translateCtx()` / `N_()` keys plus the dock-title keys in `UIPanel.h`,
  merges them into every `res/i18n/*.po`, marks removed keys obsolete
  (`#~`, preserved across runs and restored if the key comes back), and
  reports per-language coverage.
- ⚠️ **The gate only sees call sites, so it cannot catch a bare literal.** A
  `setText("Frame")` produces no key, so coverage stays at 100% and CI stays
  green while the string is permanently English. Grepping for user-visible
  literals is the only defence; the gate proves *catalogs* are complete, not
  that the code asked for a translation.
- Obsolete (`#~`) blocks are parsed like any other entry, just flagged. Skipping
  their payload made them round-trip as empty, which lost the translation, left
  the "removed-then-re-added key keeps its translation" path dead, and — because
  the rendered file then differed from disk on *every* run — reported the catalog
  as permanently stale, turning CI red for good after the first string removal.
  The invariant to preserve: **two consecutive runs must report `stale: False`
  on the second.**
- The dock-title scrape is a regex over `case PanelType::X: return "...";`, and
  it is **scoped to `UIPanel::DefaultNameKey`'s body** — `PanelIdFor()` has the
  identical switch shape but returns serialization ids, which must never enter
  the catalog (see *Layout Persistence*). Add another switch of that shape to
  `UIPanel.h` and the scope is what keeps it out.
- `python3 scripts/extract_i18n.py` → update catalogs;
  `--check` → **read-only**: writes nothing and exits 1 if a catalog is stale
  (i.e. re-running the extractor would change it) or has untranslated strings;
  `--min-coverage <pct>` → fail below a coverage threshold;
  `--verbose` → list every added/obsoleted key.
- CI runs `--check` before the build, so a missing translation or an
  uncommitted catalog update fails the PR without dirtying the tree.
- `scripts/test_extract_i18n.py` (stdlib `unittest`, no deps) tests the extractor
  itself and runs in CI *before* `--check`. It exists because a bug in the script
  does not produce a bad translation — it produces a red pipeline on every matrix
  leg that no source change can turn green, so the script needs a suite that
  names the real fault. It pins the convergence invariant above, the parser's
  tolerance of hand-edited `.po` (no blank separators, `msgid_plural`,
  `msgstr[N]`), and the `DefaultNameKey` scoping. Run it directly:
  `python3 scripts/test_extract_i18n.py`.
- The header entry (`msgid ""`) is round-tripped verbatim in canonical gettext
  form (`msgstr ""` + one quoted continuation line per field), so hand-written
  fields like `X-Language-Name` and `Plural-Forms` survive every run.
- At runtime `I18n` logs its load, e.g.
  `[I18n] loaded 160/160 strings for 'zh_CN' (0 untranslated)`.

**Adding a new translatable string:** write the English text as the msgid,
wrap it in `I18n::instance().translate("...")` (or `translateCtx`/`N_` as
appropriate), then run `scripts/extract_i18n.py` — the new key appears in the
catalog automatically and the coverage report tells you if it is translated.

**The extractor run is only half the job — a new key lands with an empty
`msgstr`, and an empty `msgstr` is a missing translation, not a pending one.**
The same commit must fill in the translation for every shipped catalog and end
with `--check` printing `OK`. Regenerating the catalog without translating it
is the trap: the file becomes *fresh* but stays incomplete, so `--check` fails
on coverage (`MISSING 24`) and CI goes red even though the working tree is
clean and the extractor reports no pending changes. Both halves are one gate,
and `make update`/`cmake` know nothing about either — run it by hand before
committing.

**Adding a new language:** drop a `<code>.po` catalog in `res/i18n/` — that is
the only step. `src/ui/CMakeLists.txt` globs `res/i18n/*.po` into
`qt_add_resources` (`CONFIGURE_DEPENDS`), and `I18n::supportedLanguages()`
enumerates the embedded `:/i18n/*.po` at runtime, taking each display name from
the catalog's `X-Language-Name` header field (falling back to
`QLocale::nativeLanguageName()`, then the code itself).

## Preferences

App-level settings (distinct from the per-project `.neurus.json` files) live
in `~/.neurus/preferences.json`, persisted with cereal JSON by the
`Preferences` struct in **`src/app/Preferences.h/cpp`** — an **Application
Layer** concept (future settings: CUDA enablement, theme, shortcut schemes).

- Fields: `language` (`"en"`/`"zh_CN"`, or `"auto"` = follow the system UI
  language) and `target_fps` (0 = unlimited).
- **`"auto"` is stored verbatim and stays that way.** `I18n::setLanguage()`
  resolves the sentinel itself, so nothing on the load path rewrites the
  preference with a concrete code — resolving it in place would turn "follow the
  system language" into "pin to whatever the system said at first launch" the
  moment the file was next written.
- The **Application is the sole manager**: it loads the file before the window is
  built, applies the saved language and target FPS, saves on every edit, and
  saves once more on `aboutToQuit`.
- **Loading distinguishes missing from corrupt** (`Preferences::LoadResult`), and
  never mutates the fields on either failure — parsing goes into a scratch copy,
  so a truncated or schema-drifted file cannot leave values half-applied:
  - `Ok` — values in effect.
  - `Missing` — first run; the Application writes the defaults.
  - `Corrupt` — the Application logs it, keeps defaults, and calls
    `Preferences::Backup()` to move the file to `preferences.json.bak`. It must
    **not** save instead: that would destroy still-recoverable settings, and the
    move also stops the `aboutToQuit` save from clobbering the evidence.
- **The UI never touches the `Preferences` type.** The Application seeds the
  window with plain values (`UIManager(language, targetFps, prefsPath)`); the
  `PreferencesDialog` (File → Preferences… / `Ctrl+,`) is a pure UI widget
  holding only those plain values and emits `languageChangeRequested(QString)`
  / `targetFpsChangeRequested(int)` → `UIEvents` → Application applies +
  persists + (for language) calls `I18n::setLanguage()`, which fans out the
  live retranslation. Changes apply immediately; the dialog only has Close
  (no OK/Cancel).
- The language combo's **row 0 is "System default"**, carrying `"auto"` as item
  data — `I18n::supportedLanguages()` lists catalogs only, so without that row
  the shipped default would be unreachable from the UI. It is the only row
  Retranslate() touches; the rest name their own language.
- "Reset to Defaults" resets to `"auto"` + 60 FPS — the shipped defaults, not
  the currently detected system code.

## Build Integration

```cmake
# ADS submodule (static build)
set(ADS_VERSION "4.5.0")
set(BUILD_EXAMPLES OFF)
set(BUILD_STATIC ON)
add_subdirectory(dep/qtadvanceddocking)

# Link targets
target_link_libraries(Neurus PRIVATE ads::qtadvanceddocking-qt6 ...)
target_compile_definitions(Neurus PRIVATE ADS_STATIC)
```


## Panel System

All dock panels inherit from `UIPanel` (`src/ui/panels/UIPanel.h`):
- `PanelType` enum identifies each panel (Viewport, Outliner, PropertyEditor, RenderConfig, Log)
- `virtual void Refresh(const UIContext& ctx)` — called per-frame to sync panel UI with application state
- `UIContext` embeds an `EditorContext editor` (from `Editor::GetContext()`) with
  `editor.scene` (opaque `const UID*`, cast to `const Scene*`) and `editor.config`
  (opaque `const void*`, cast to `const RenderConfig*`)
- `UIContext` also carries a `const void* profile` pointer to the `FrameProfile`
  returned by `DeferredRenderer::DrawFrame()`. The Application assembles the
  UIContext each frame (the Editor never produces it) and the ProfilingPanel casts
  `profile` to `const FrameProfile*` to render a real-time per-pass tree (Frame
  totals + per-pass rows). Collection is always active while the app runs
  (`Application::InitRenderer` enables it)
- `UIManager` stores panels in `std::map<PanelType, ads::CDockWidget*> m_panelDocks`

### RenderConfigPanel

`RenderConfigPanel` provides live-adjustable render settings organized in collapsible `QGroupBox` sections:
- **Shadows**: Algorithm (None/ShadowMapping/SDFSoftShadow/VSSM), PCF filter mode, bias `ScalarSlider` (0.0–0.1, derived step 0.0001, 4 decimals)
- **Ambient Occlusion**: Algorithm (None/SSAO), kernel size spinbox, radius `ScalarSlider` (0.0–5.0)
- **Lighting**: IBL toggle, exposure `ScalarSlider` (0.0–5.0)
- **Post-Processing**: Anti-aliasing combo, gamma `ScalarSlider` (1.0–3.0)
- **Pipeline**: Pipeline type (Forward/Deferred), SSR mode, samples per frame

Each control change emits `configValueChanged(RenderConfig cfg)`, wired by `Application` to `Editor::SetRenderConfig(cfg)`.

> **Not yet exposed (issue #22 follow-up):** `RenderConfig::r_debug_draw` — the master
> switch for the viewport debug/gizmo overlay — has no control in this panel yet. It
> defaults to `true` and is only reachable by editing a project file. When adding it,
> a plain checkbox in a new **Overlay** group is enough: the flag gates *publication*,
> not graph topology, so toggling it needs no RenderGraph rebuild. The same follow-up
> covers the missing `GO_DL` / `GO_DP` / `GO_DM` cases in `Icons::ObjectIcon` (they
> currently fall through to the mesh icon), the Outliner add-menu entries for the
> three debug object types, and their PropertyEditor panels.

### ProfilingPanel

`ProfilingPanel` (Bottom dock) shows the per-frame GPU/CPU profile returned by
`DeferredRenderer::DrawFrame()` as a `QTreeWidget` (columns: Pass, CPU (ms),
GPU (ms), Draws, Dispatches):
- A bold **Frame** root row summarizes the whole frame (CPU record ms, GPU frame
  ms, total draws/dispatches)
- One child row per pass (`PassProfile`) shows that pass's CPU/GPU ms and
  draw/dispatch counts
- `Refresh()` rebuilds the tree only when the frame totals change; GPU columns
  show `--` until timestamp readback is ready (device support + fence signal)
- Profiling is always on; the panel is display-only

### LogPanel

`LogPanel` (Bottom dock, tabbed with Texture Viewer) is a realtime log viewer
reading the core `LogBuffer` via `UIContext::log` (opaque `const LogBuffer*`,
polled in `Refresh()`):

- **Row rendering**: `QListView` + `LogModel` (custom `QAbstractListModel` over
  the buffer) + `LogFilterProxy` (level filter + case-insensitive search) +
  `LogDelegate` (severity coloring: cyan Info, red Error). Virtualized - only
  visible rows are queried.
- **Counts**: `QLabel` header "INFO n · ERROR m", dirty-checked in `Refresh()`.
- **Controls**: filter combo (All/Info/Errors), search box, auto-scroll toggle
  (default on), pause toggle, Clear (resets the core buffer), Export... (`*.log`
  via `QFileDialog` + `std::ofstream`).
- **Error notifier**: on new `NEURUS_ERR` entries, LogPanel emits
  `errorNotified(delta, firstMessage)`; UIManager shows a 3s status-bar message
  ("N new error(s): <first>"). Throttled - repeated errors accumulate in delta.
- **Release behavior**: `NEURUS_LOG` compiles out, so only `NEURUS_ERR` lines
  appear and the count reflects that.

### Outliner, Viewport, and PropertyPanel (Scene-Event Panels)

`Outliner`, `Viewport`, and `PropertyPanel` emit scene-domain events that
carry plain **integer object UIDs** (`int objectUid`, 0 = none). Panels hold
the active object's UID as an `int` across `Refresh()` calls and compare the
ints for dirty checks and lazy updates — no cached object pointers (a raw
pointer could dangle after the object is deleted; a UID stays stable). Panels
emit PURE INPUT INTENTS on the UI->Editor path - they no longer hold or
stamp the scene (`m_scene` is gone). The Editor wraps the intents into
complete scene events (see events.instructions.md, "Three event paths").

- `Outliner::Refresh()` reads the object UID list from `UIContext` and
  reconfigures pooled `OutlinerRow`s via `setObject()` /
  `setVisibilities()` / `setSelectionMode()`. The panel emits
  `objectClicked(const ObjectClicked&)`, `visibilityChanged(const
  VisibilityChanged&)`, and `deleteRequested(const DeleteRequested&)`.
- `OutlinerRow` emits `ObjectClicked{m_objectUid, mods}`; signal lambdas
  read the row's `m_objectUid` (an `int`) at emission time, so pooled rows
  stay correct when recycled to a different object.
- `Viewport` emits the raw mouse-input intents (`mouseMoved`,
  `mousePressed`, `mouseReleased`, `mouseScrolled`) plus
  `deleteRequested(const DeleteRequested&)` on the Delete key.
- `PropertyPanel::Refresh()` reads `scene->selections.GetActiveObject()`,
  stores its `int` UID (lazy header update), and emits transform events
  (`PositionChanged`, `RotationChanged`, `ScaleChanged`) plus
  camera/mesh/light/environment/debug property events, all carrying the active
  object's `int objectUid`; it resolves per-id data via `Scene::GetObjectID`.
  The debug subpanel is shared by GO_DL/GO_DP/GO_DM: `Refresh()` calls
  `setObjectId()` **before** the setters (it invalidates the caches) and
  `setDebugType()` to pick the visible rows. The position list is flattened to
  xyz triples in the forwarding lambda, since `SceneEvents.h` stays glm-free.

### ShaderEditorPanel

`ShaderEditorPanel` (Left dock) edits the GLSL shader of the active mesh in two
modes selected by a `QComboBox`:
- **Code mode**: raw GLSL in a `CodeEditor` widget (line numbers, GLSL syntax
  highlighting). Text changes emit `codeEdited`.
- **Structure mode**: a `QTreeView` (`ShaderStructModel` + `ShaderFieldDelegate`)
  showing the parsed `ShaderStruct` IR as a 3-level tree
  (sections → fields/structs → members). Type/name cells are edited inline via
  `QComboBox` / `QLineEdit` editors and emit `structEdited`.

Each section header row and each struct-definition row carries a "+" glyph
painted at the row's right edge by `ShaderFieldDelegate` (no per-row index
widgets — `QTreeView::setIndexWidget` fought with the frequent model resets and
macOS accessibility, swallowing input after a click). Clicking it appends a new
entry to that section (or a member field to that struct) and emits
`fieldAdded`. A toolbar "−" removes the selected entry. Both "Create Shader"
(no shader yet) and "Compile" buttons are available; Compile
re-parses/generates/compiles and bumps the shader version so the GeometryPass
pipeline rebuilds.

**State preservation across `Refresh()`:** the tree model is rebuilt on every
shader version change (`setShaderStruct` → model reset), which would wipe
expansion, selection, column spans, and keyboard focus. `populateSections()`
captures the expanded rows, current selection, and focus before the rebuild and
restores them after, then re-applies the spans. Opened sections stay open,
closed sections stay closed, the selected row survives edits/adds, and window
shortcuts (Ctrl+Z) keep working after a "+" click.

Signals (`createShaderRequested`, `compileRequested`, `codeEdited`,
`structEdited`, `fieldAdded`) are wired by `Application` to `Editor` and
dispatched to `ShaderController` (see events.instructions.md).

## Reusable Items

Items in `src/ui/items/` are self-contained composite QWidgets used across panels.
Qt model/view helpers live in `src/ui/models/` (QAbstractItemModel subclasses)
and `src/ui/delegates/` (QStyledItemDelegate subclasses); non-widget helpers live
in `src/ui/utils/`. They do NOT belong to any specific panel and should be reused
rather than redefined. For type-specific property editors built from items, see
[Property Presets](#property-presets) in `src/ui/presets/`.

### Icons

`Icons` (`src/ui/Icons.h`) is a fully static class providing lazy-loaded QIcon objects:

- **`Icons::Initialize()`** — called once by `UIManager` during construction. Populates the hardcoded path registry (icon name → Qt resource path). Safe to call multiple times (idempotent).
- **`Icons::GetIcon(name)`** — returns `const QIcon&` from an internal cache. On first access, loads the SVG from the Qt resource system; subsequent calls return the cached instance.
- **Naming convention**: `"folder:name"` (e.g. `"scene:mesh"` → `:/icons/scene/mesh.svg`). 10 icons registered across `scene:` and `editor:` namespaces.
- **Path registry**: hardcoded in `Initialize()`, mapping icon names to `:/icons/...` Qt resource paths.
- **Cache**: `std::unordered_map<std::string, QIcon>`, populated lazily per icon name.
- **No instantiation needed** — all members are static. Any code in `src/ui/` can call `Icons::GetIcon(name)` directly.

Icons are embedded in the binary via `qt_add_resources(neurus_ui "icons" PREFIX "/icons" ...)` in `src/ui/CMakeLists.txt`. SVGs live under `res/ui/icons/`.

### ScalarSlider

`ScalarSlider` (`src/ui/items/ScalarSlider.h`) is a reusable composite `QWidget`:
- Pairs a `QSlider` (int) with a `QDoubleSpinBox` in a `QHBoxLayout`
- Bidirectional sync with `blockSignals` to prevent feedback loops
- Emits a single `valueChanged()` signal regardless of which control moved
- Step and decimals auto-derived: `step = (max-min)/sliderSteps`, `decimals = ceil(-log10(step))`
- Tick marks enabled with interval = `max(1, sliderSteps/10)`
- `setValue()` dirty-checks against `m_v`, which mirrors **what the controls
  currently show — user edits included** (both sync lambdas write it). A shadow
  copy that tracked only programmatic writes would go stale the moment the user
  touched the control, and a later push of that same value would then be silently
  dropped while the widget still displayed the user's number — i.e. a panel could
  never resynchronise the widget after an undo.
- `pressed()` / `released()` bracket the whole mouse gesture. They come from an
  event filter on the slider, not from `sliderPressed`/`sliderReleased`, which
  miss groove clicks and would split a click+drag into two undo entries.

### Vec3Spin

`Vec3Spin` (`src/ui/items/Vec3Spin.h`) is a reusable composite `QWidget`:
- Three `QDoubleSpinBox` widgets in a horizontal row (X, Y, Z)
- Configurable range, step, decimals, and suffix
- Emits `valueChanged(x, y, z)` signal when any spinbox changes
- `setValue(x, y, z)` with internal dirty-check — no-ops if all three values unchanged
- Uses `QSignalBlocker` internally to prevent feedback loops during programmatic updates


## Property Presets

Presets in `src/ui/presets/` are the GOType-specific subpanels of
`PropertyPanel`: `CameraProperties`, `MeshProperties`, `LightProperties`,
`EnvironmentProperties` and `DebugProperties`. `PropertyPanel` owns one instance
of each, shows exactly one per selection (`ShowTypeSubpanel`), and forwards their
signals as typed scene events.

Every preset follows the same contract:
- A `QWidget` holding **one `QGroupBox`** — no dock, no scene access, no Vulkan.
- `setObjectId(int)` binds the panel and **resets the dirty-check caches**, so
  the first push after a selection change always writes through even when the new
  object's value equals the old one's.
- Every `set*()` is **dirty-checked** against a cached value and writes through
  `blockSignals`/`QSignalBlocker`, because `Refresh()` re-pushes the scene's
  values every frame and must not fight the widget the user is editing.
- Signals carry `(int objectId, value)`; `PropertyPanel` wraps them into
  `SceneEvents` structs stamped with `m_activeObjectId`.
- `Retranslate()` re-applies every label via `I18n::instance().translate(...)`.
- Rows that apply to only some subtypes are wrapped in a plain `QWidget*` row and
  toggled with `setVisible` (spot-cone rows for a spot light; line/point rows for
  a debug type).

### DebugProperties (issue #22)

One preset serves all three debug types — `DebugLine`, `DebugPoints` and
`DebugMesh` share color / opacity / x-ray, and `setDebugType(int goType)` shows
or hides the type-specific rows (line width + stipple + smoothing; point shape +
size + projection mode; mesh path label).

Its **position table** is the only place in the panel that edits a *list* rather
than scalars, and is worth knowing about:
- Edited **absolutely**: a cell edit, an Add and a Remove all emit the whole list
  via `positionsChanged`. A structural change cannot be expressed per index, and
  one event shape keeps undo to a single op.
- An unparseable cell reads back as the coordinate it replaced and emits nothing,
  so a typo mid-edit cannot teleport a vertex to the origin.
- `m_populating` guards the emit path while `setPositions()` repopulates, so a
  programmatic fill never looks like a user edit.
- Beyond `kMaxRows` (512) the table shows the first 512 rows, goes
  `NoEditTriggers` and disables Add/Remove — writing back only the visible rows
  would silently drop the hidden tail. The count label still reports the true
  size.
- `DebugMesh` hides the table entirely: its geometry is a pooled `MeshData`, not
  an editable list.


## Patterns

### Loading QSS Stylesheets from Qt Resources

Stylesheets should be stored as `.qss` files in `src/ui/qml/` and embedded via
`qt_add_resources`, same as QML. Use `setObjectName()` + QSS ID selectors
instead of per-widget `setStyleSheet()` calls:

```
// 1. Embed in CMakeLists.txt
qt_add_resources(neurus_ui "resources" PREFIX "/" FILES qml/outliner.qss)

// 2. Load once (static, one-shot)
QFile file(":/qml/outliner.qss");
file.open(QIODevice::ReadOnly | QIODevice::Text);
QString stylesheet = QTextStream(&file).readAll();

// 3. Apply to parent widget — ID selectors cascade to children
parentWidget->setStyleSheet(stylesheet);

// 4. In C++, set object names matching QSS ID selectors
childBtn->setObjectName("myButton");  // matches QPushButton#myButton

// 5. Per-instance overrides via setStyleSheet() on the specific child
childBtn->setStyleSheet("QPushButton { color: #ff6f00; }");
```

See `src/ui/qml/outliner.qss` for the canonical example using
`QPushButton#outlinerNameBtn` and `QPushButton#outlinerToggleBtn` selectors.

## Performance Optimization

### Lazy Updates via Logical State Tracking

Avoid redundant widget operations by storing the last-known state and skipping
updates when nothing changed:

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

This applies to any `Refresh()`-based panel — cache the previous value of each
widget-modifying input and gate the write behind an equality check.

### Prefer QSS Over Inline Stylesheets

Setting `setStyleSheet()` on individual widgets forces Qt to re-parse the CSS
text and re-resolve all selectors, which is measurably slower than QSS class
selectors with dynamic properties:

```cpp
// DO: QSS with dynamic property (fast, declarative)
//  .qss: QPushButton#outlinerNameBtn[selectionState="active"] { color: #ff6f00; }
m_nameBtn->setProperty("selectionState", "active");
m_nameBtn->style()->unpolish(m_nameBtn);
m_nameBtn->style()->polish(m_nameBtn);

// DON'T: inline stylesheet (slow, re-parses CSS text each time)
m_nameBtn->setStyleSheet("QPushButton { color: #ff6f00; }");
```

The same principle applies to `setIcon()` on toggle buttons — use
`GetIconPair()` to pre-bake On/Off states into a single `QIcon` instead of
string-building icon names and calling `GetIcon()` on every toggle.

### Naming Convention: `set*` for Refresh-Path Methods

Widget methods called from `Refresh()` or other per-frame update paths must
use Qt-style lowercase naming (e.g. `setObject()`, `setValue()`, `setRowIndex()`)
rather than PascalCase. This aligns with Qt's own convention for setter methods
(`QSpinBox::setValue()`, `QWidget::setVisible()`) and distinguishes them from
application-level PascalCase methods (`BuildTransformEditor()`, `PopulateTransform()`).

New reusable widgets in `src/ui/items/` (and models/delegates/utils) should
follow this convention for all public setters that participate in the Refresh
pipeline.

### ✅ UI MAY:
- Own QVulkanInstance, VulkanWindow, QMainWindow
- Emit EventBus signals
- Handle Qt events (resize, close, menu actions)
- Manage ADS dock layout

### ❌ UI MUST NOT:
- Directly call Renderer methods (go through EventBus)
- Create Vulkan objects beyond QVulkanInstance
- Mutate scene state directly
- Access GPU resources directly
- Redefine the UI elements that are already in `ui/items`

## Legacy Code

The following files are retained as reference implementations but are no longer the primary rendering path:
- `VulkanWidget.h/cpp` - vk::raii surface via native HWND
- `MainWindow.h/cpp` - original QWindow subclass
- `main.qml` - original QML window
- `Renderer.h/cpp` - vk::raii render loop with dynamic rendering
- `Swapchain.h/cpp` - manual vk::raii swapchain management
