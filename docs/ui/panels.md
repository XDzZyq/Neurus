# UI Panels

Dock panels are the widgets hosted inside ADS docks; every scene-facing panel inherits `UIPanel` and syncs itself from the per-frame `UIContext`.

## UIPanel base

`UIPanel` (`src/ui/panels/UIPanel.h`) is the common `QWidget` base for all dockable panels, so the dock system can treat them uniformly:

- `PanelType` enum identifies each panel: `Viewport`, `Outliner`, `PropertyPanel`, `RenderConfig`, `ShaderEditor`, `Profiling`, `Log` (plus a `Count` sentinel that is never a panel's type).
- `GetPanelType()` returns the enum for O(1) lookup in the panel registry.
- `PanelName()` provides the human-readable dock title, translated through `I18n` in the `"Dock"` context at call time.
- `PanelId()` / `PanelIdFor(PanelType)` provide the stable serialization key, which is never translated. `PanelId()` is the panel's **identity** - ADS uses the dock's `objectName` as the key when saving and restoring layouts, so these strings are part of the project file format and are treated as frozen. `PanelName()` is **display only** and must never be persisted or used as a lookup key.

- `virtual void Refresh(const UIContext& ctx) = 0` - called by `UIManager::Refresh()` each frame to extract the data a panel needs from the context and update its widgets.
- `virtual void Retranslate()` - re-applies user-visible text in the active language. Panels call it once at the end of their constructor (so the startup language applies) and the framework calls it again on every language change. Panels without translatable text (e.g. `Viewport`) keep the default no-op.

The dock title is stored as a translation *key* (`nameKey`, default derived from `PanelType`); `PanelName()` resolves it through `I18n`. See [i18n](i18n.md) for the translation pipeline.

## Viewport

`Viewport` (`src/ui/panels/Viewport.h`) is a `QWidget` that exposes a native window handle for the surface:

- Sets `WA_NativeWindow` and `WA_OpaquePaintEvent`, so the widget has a dedicated native handle and Qt does not draw a background behind the Vulkan content.
- Overrides `paintEvent()` to a no-op - all rendering is handled by Vulkan.
- Is focusable (`Qt::StrongFocus`) to receive keyboard input.
- Owns no Vulkan resources; `hwnd()` returns the native handle and surface creation is the renderer's responsibility.

Signals:

- `resized(int, int)` - lets the renderer recreate the swapchain with the new dimensions.
- `mouseMoved`, `mousePressed`, `mouseReleased`, `mouseScrolled` - raw mouse-input intents translated to typed event structs. The viewport tracks its own button state and mouse position internally.
- `deleteRequested(const DeleteRequested&)` on the Delete key - a pure intent; the Editor deletes all selected objects.
- `keyPressed(const KeyPressEvent&)` - only keys the input layer recognises are forwarded, and auto-repeat is filtered at the Qt boundary so a held key arms a modal operator once. This is deliberately not a `QShortcut`: a `Qt::WindowShortcut` on G/R/S/X/Y/Z would steal those letters from the shader editor and from every property spinbox, and cannot filter auto-repeat.
- `focusLost(const ViewportFocusLost&)` - a modal transform gesture is driven by viewport keys and the viewport cursor, so clicking into another panel mid-gesture must abort it rather than leave an invisible operator armed against a widget that no longer receives its keys.
- `focusOutEvent` deliberately ignores `ActiveWindowFocusReason` and `PopupFocusReason`: a modal gesture only pauses for those reasons, and aborting would revert in-progress transforms on an app switch, an IME popup or a notification.

`Refresh()` is pure input forwarding plus Vulkan rendering; the panel holds no scene state. Mouse/Delete events are pure intents - the Editor wraps the active scene and forwards dedicated events to controllers.

## Outliner

`Outliner` (`src/ui/panels/Outliner.h`) is a Blender-style vertical list of scene objects; each row shows the object type icon, name, and two visibility toggles (viewport / render). It reads scene data via `UIContext` with no Renderer or Vulkan headers.

- `Refresh()` reads the object UID list from `UIContext` and reconfigures pooled `OutlinerRow`s via `setObject()` / `setVisibilities()` / `setSelectionMode()`. The row pool grows when scene objects exceed pool size and recycles existing rows otherwise; extra rows are hidden, never destroyed.
- `OutlinerRow` emits `ObjectClicked{m_objectUid, mods}`; signal lambdas read the row's `m_objectUid` at emission time, so pooled rows stay correct when recycled to a different object.
- The panel emits `objectClicked(const ObjectClicked&)`, `visibilityChanged(const VisibilityChanged&)` and `deleteRequested(const DeleteRequested&)` (Delete key).
- `Retranslate()` re-applies the category-group title; `EnsureRowPool()` / `RetranslateRow()` make rows created *after* a language switch carry the translated visibility tooltips.

See [Items](items.md) for `OutlinerRow`.

## PropertyPanel

`PropertyPanel` (`src/ui/panels/PropertyPanel.h`) shows a type/name header, an editable transform, and a type-specific subpanel for the active scene object.

- `Refresh()` reads `scene->selections.GetActiveObject()`, stores its integer UID (lazy header update), and displays its name with type icon and populates the Position / Rotation / Scale `Vec3Spin` widgets from its `Transform3D`. The header shows an empty state when nothing is selected.
- It emits granular transform events (`PositionChanged`, `RotationChanged`, `ScaleChanged`) plus camera/mesh/light/environment/debug property events, all carrying the active object's `int objectUid`; it resolves per-id data via `Scene::GetObjectID`.
- It owns one instance of each property preset (camera, mesh, light, environment, debug), shows exactly one per selection (`ShowTypeSubpanel`), and forwards their signals as typed scene events.
- The debug subpanel is shared by `GO_DL` / `GO_DP` / `GO_DM`: `Refresh()` calls `setObjectId()` **before** the setters (it invalidates the caches) and `setDebugType()` to pick the visible rows. The position list is flattened to xyz triples in the forwarding lambda, since `SceneEvents.h` stays glm-free.

**`SetEnabled(false)` must not hide the type subpanels - `ShowTypeSubpanel()` alone decides which one is visible.** `Refresh()` runs every frame and calls `SetEnabled()` *before* `ShowTypeSubpanel()`, so hiding the subpanels inside `SetEnabled(false)` hid the visible one and the next line re-showed it. For an object whose `GetTransform()` is null that hide/show pair fired **every frame**; the transient hide stripped focus from any control in the panel and Qt re-homed it on the `Viewport`, so the color swatch flickered between hoverable and not and the picker never kept focus. Hiding all subpanels when nothing is selected is instead done explicitly on `Refresh()`'s two early-return paths.

### Transformable debug objects

`DebugLine` and `DebugPoints` are transformable like any other scene object. Both inherit `Transform3D` and override `GetTransform()` to return themselves, so the gizmo's G/R/S and the panel's Transform group act on them, and the debug draw builder bakes `GetModelMatrix()` into the world-space overlay payload. Their stored vertex/point list is therefore **local space, relative to the transform**: the Positions table edits local coordinates, and moving the transform moves the whole line/point set. `Sprite` remains the only scene type with no transform.

### Property presets

Presets in `src/ui/presets/` are the GOType-specific subpanels of `PropertyPanel`: `CameraProperties`, `MeshProperties`, `LightProperties`, `EnvironmentProperties` and `DebugProperties`. `PropertyPanel` owns one instance of each, shows exactly one per selection, and forwards their signals as typed scene events.

Every preset follows the same contract:

- A `QWidget` holding **one `QGroupBox`** - no dock, no scene access, no Vulkan.
- `setObjectId(int)` binds the panel and **resets the dirty-check caches**, so the first push after a selection change always writes through even when the new object's value equals the old one's.
- Every `set*()` is **dirty-checked** against a cached value and writes through `blockSignals` / `QSignalBlocker`, because `Refresh()` re-pushes the scene's values every frame and must not fight the widget the user is editing.
- Signals carry `(int objectId, value)`; `PropertyPanel` wraps them into `SceneEvents` structs stamped with the active object id.
- `Retranslate()` re-applies every label via `I18n::instance().translate(...)`.
- Rows that apply to only some subtypes are wrapped in a plain `QWidget*` row and toggled with `setVisible` (spot-cone rows for a spot light; line/point rows for a debug type).
- `CameraProperties` edits target plus FOV; `MeshProperties` a path label plus shadow/material checkboxes; `LightProperties` a type label plus power/radius sliders and a shadow checkbox; `EnvironmentProperties` a path label plus intensity/rotation sliders.

#### DebugProperties

One preset serves all three debug types - `DebugLine`, `DebugPoints` and `DebugMesh` share color / opacity / x-ray, and `setDebugType(int goType)` shows or hides the type-specific rows (line width + stipple; point shape + size + projection mode; mesh path label). Antialiasing is deliberately **not** a row: the debug draw builder derives it per primitive kind (solid line and cube edge = smooth, dashed line = not, sprite = always, wire mesh = never), so there is nothing for the user to set.

Its **position table** is the only place in the panel that edits a *list* rather than scalars:

- Edited **absolutely**: a cell edit, an Add and a Remove all emit the whole list via `positionsChanged`. A structural change cannot be expressed per index, and one event shape keeps undo to a single op.
- An unparseable cell reads back as the coordinate it replaced and emits nothing, so a typo mid-edit cannot teleport a vertex to the origin.
- `m_populating` guards the emit path while `setPositions()` repopulates, so a programmatic fill never looks like a user edit.
- Beyond `kMaxRows` (512) the table shows the first 512 rows, goes `NoEditTriggers` and disables Add/Remove - writing back only the visible rows would silently drop the hidden tail. The count label still reports the true size.
- `DebugMesh` hides the table entirely: its geometry is a pooled `MeshData`, not an editable list.

## RenderConfigPanel

`RenderConfigPanel` (`src/ui/panels/RenderConfigPanel.h`) provides live-adjustable render settings organized in collapsible `QGroupBox` sections:

- **Shadows**: Algorithm (None / ShadowMapping / SDFSoftShadow / VSSM), PCF filter mode, bias `ScalarSlider` (0.0-0.1, derived step 0.0001, 4 decimals), temporal sampling mode.
- **Ambient Occlusion**: Algorithm (None / SSAO), kernel size spinbox, radius `ScalarSlider` (0.0-5.0).
- **Lighting**: IBL toggle, translucency toggle, exposure `ScalarSlider` (0.0-5.0).
- **Post-Processing**: Anti-aliasing combo, gamma `ScalarSlider` (1.0-3.0), FXAA sliders.
- **Pipeline**: Pipeline type (Forward / Deferred), SSR mode, samples per frame.

Each control change emits `configValueChanged(RenderConfigChangedEvent)`, wired by the Application to `Editor::SetRenderConfig(cfg)`. A bounded slider edit begin/end (`editBegin` / `editEnd`) bounds one undo entry for continuous slider drags.

> **Not yet exposed (issue #22 follow-up):** `RenderConfig::r_debug_draw` - the master switch for the viewport debug/gizmo overlay - has no control in this panel yet. It defaults to `true` and is only reachable by editing a project file. When adding it, a plain checkbox in a new **Overlay** group is enough: the flag gates *publication*, not graph topology, so toggling it needs no render graph rebuild. The same follow-up covers the missing `GO_DL` / `GO_DP` / `GO_DM` cases in `Icons::ObjectIcon` (they currently fall through to the mesh icon), the Outliner add-menu entries for the three debug object types, and their PropertyPanel panels.

## ProfilingPanel

`ProfilingPanel` (`src/ui/panels/ProfilingPanel.h`) shows the per-frame GPU/CPU profile returned by the renderer as a `QTreeWidget` (columns: Pass, CPU (ms), GPU (ms), Draws, Dispatches):

- A bold **Frame** root row summarizes the whole frame (CPU record ms, GPU frame ms, total draws/dispatches).
- One child row per pass shows that pass's CPU/GPU ms and draw/dispatch counts.
- `Refresh()` rebuilds the tree only when the frame totals change; GPU columns show `--` until timestamp readback is ready (device support + fence signal). GPU timestamps lag the CPU data by 1-2 frames by design.
- Displayed timings are smoothed with a per-value exponential moving average so the numbers do not jitter frame to frame; draw/dispatch counts are shown raw.
- Pass rows are pooled and never destroyed, mirroring the Outliner row pool - no per-frame item allocation. Columns are dirty-checked (see the lazy-update convention).
- Profiling is always on; the panel is display-only and reads the Vulkan-free profile POD, with no Vulkan or Renderer headers.

See [Items](items.md) for `ProfilingRow` / `ProfilingHead`.

## ShaderEditorPanel

`ShaderEditorPanel` (`src/ui/panels/ShaderEditorPanel.h`) edits the GLSL shader of the active mesh in two modes selected by a `QComboBox` in a `QStackedWidget`:

- **Code mode**: raw GLSL in a `CodeEditor` widget (line numbers, GLSL syntax highlighting). Text changes emit `codeEdited`.
- **Structure mode**: a `QTreeView` (`ShaderStructModel` + `ShaderFieldDelegate`) showing the parsed `ShaderStruct` IR as a 3-level tree (sections -> fields/structs -> members). Type/name cells are edited inline via `QComboBox` / `QLineEdit` editors and emit `structEdited`.

Each section header row and each struct-definition row carries a "+" glyph painted at the row's right edge by `ShaderFieldDelegate` (no per-row index widgets - `QTreeView::setIndexWidget` fought with the frequent model resets and macOS accessibility, swallowing input after a click). Clicking it appends a new entry to that section (or a member field to that struct) and emits `fieldAdded`. A toolbar "-" removes the selected entry. Both "Create Shader" (no shader yet) and "Compile" buttons are available; Compile re-parses/generates/compiles and bumps the shader version so the geometry-pass pipeline rebuilds.

**State preservation across `Refresh()`:** the tree model is rebuilt on every shader version change (`setShaderStruct` -> model reset), which would wipe expansion, selection, column spans and keyboard focus. `populateSections()` captures the expanded rows, current selection and focus before the rebuild and restores them after, then re-applies the spans. Opened sections stay open, closed sections stay closed, the selected row survives edits/adds, and window shortcuts (Ctrl+Z) keep working after a "+" click.

Restoring the tree's keyboard focus after the model reset is load-bearing on macOS: without it the accessibility layer chases the dangling focus and swallows key input.

`Refresh()` uses the same integer-UID pattern as the other scene panels: it keeps the active mesh UID and caches the stage type and shader version for lazy updates, and shows an empty state or the "Create Shader" button as appropriate.

Signals (`createShaderRequested`, `compileRequested`, `codeEdited`, `structEdited`, `fieldAdded`, `editBegin`, `editEnd`) are wired by the Application to the Editor and dispatched to a shader controller. See [Editor events](../editor/events.md).

See [Models](models.md) for `ShaderStructModel` and `ShaderFieldDelegate`.

## LogPanel

`LogPanel` (`src/ui/panels/LogPanel.h`) is a realtime log viewer reading the core `LogBuffer` via `UIContext::log` (a typed `const LogBuffer*`, polled in `Refresh()`):

- **Row rendering**: `QListView` + `LogModel` (custom `QAbstractListModel` over the buffer) + `LogFilterProxy` (level filter + case-insensitive search) + `LogDelegate` (severity coloring: red Error, dark gray Info). Virtualized - only visible rows are queried.
- **Counts**: a `QLabel` header "INFO n / ERROR m", dirty-checked in `Refresh()`.
- **Controls**: filter combo (All / Info / Errors), search box, auto-scroll toggle (default on), pause toggle, Clear (resets the core buffer), Export... (`*.log` via `QFileDialog` + `std::ofstream`).
- **Error notifier**: on new `NEURUS_ERR` entries, `LogPanel` emits `errorNotified(delta, firstMessage)`; `UIManager` shows a 3s status-bar message ("N new error(s): <first>"). Throttled - repeated errors accumulate in the delta.
- **Release behavior**: `NEURUS_LOG` compiles out, so only `NEURUS_ERR` lines appear and the count reflects that.

See [Models](models.md) for `LogModel`, `LogFilterProxy` and `LogDelegate`.

## PreferencesDialog

`PreferencesDialog` (`src/ui/panels/PreferencesDialog.h`) is opened from **File -> Preferences...** (`Ctrl+,`). It is a pure UI widget:

- It is seeded with plain values (language code, target FPS, preferences-file path) and never touches the app-layer `Preferences` type - the Application is its sole manager.
- It emits `languageChangeRequested(QString)` / `targetFpsChangeRequested(int)`, routed through `UIEvents` so the Application applies the change, persists `~/.neurus/preferences.json`, and (for language) calls `I18n::setLanguage()`, which fans out the live retranslation.
- All changes apply immediately; the dialog only has Close (no OK/Cancel). `SyncFrom(language, targetFps)` re-seeds the controls without emitting.
- It connects to `I18n::languageChanged()` so it retranslates while open.
- The language combo's **row 0 is "System default"**, carrying `"auto"` as item data - `I18n::supportedLanguages()` lists catalogs only, so without that row the shipped default would be unreachable from the UI. It is the only row `Retranslate()` touches; the rest name their own language.
- "Reset to Defaults" resets to `"auto"` + 60 FPS - the shipped defaults, not the currently detected system code.

See [overview](overview.md#preferences) for the app-level preferences model and [i18n](i18n.md) for language switching.
