# UI Items

Items in `src/ui/items/` are self-contained composite `QWidget`s used across panels; they belong to no specific panel and should be reused rather than redefined.

For Qt model/view helpers see [Models](models.md); for type-specific property editors built from items, see [Panels](panels.md#property-presets) (`src/ui/presets/`); non-widget helpers live in `src/ui/utils/`.

## Icons

`Icons` (`src/ui/Icons.h`) is a fully static class providing lazy-loaded `QIcon` objects:

- **`Icons::Initialize()`** - called once by `UIManager` during construction. Populates the hardcoded path registry (icon name -> Qt resource path). Safe to call multiple times (idempotent).
- **`Icons::GetIcon(name)`** - returns `const QIcon&` from an internal cache. On first access it loads the SVG from the Qt resource system; subsequent calls return the cached instance.
- **`Icons::GetIconPair(on, off)`** - pre-bakes an On/Off state pair into a single `QIcon`, so a toggle button does not string-build icon names and reload on every toggle.
- **`Icons::ObjectIcon(goType)`** - returns the type icon for a scene object `GOType` (1=MESH, 3=LIGHT, 4=CAMERA, others fall back to MESH).
- **Naming convention**: `"folder:name"` (e.g. `"scene:mesh"` -> scene/mesh.svg). The registry currently maps 10 icons across the `scene:` and `editor:` namespaces.
- **Cache**: `std::unordered_map<std::string, QIcon>`, populated lazily per icon name.
- **No instantiation needed** - all members are static, so any code in `src/ui/` can call `Icons::GetIcon(name)` directly.

Icons are embedded in the binary via `qt_add_resources(neurus_ui "icons" PREFIX "/icons" ...)` in `src/ui/CMakeLists.txt`; SVGs live under `res/ui/icons/`.

## ScalarSlider

`ScalarSlider` (`src/ui/items/ScalarSlider.h`) is a reusable composite `QWidget`:

- Pairs a `QSlider` (int) with a `QDoubleSpinBox` in a `QHBoxLayout`.
- Bidirectional sync with `blockSignals` to prevent feedback loops.
- Emits a single `valueChanged()` signal regardless of which control moved.
- Step and decimals are auto-derived: `step = (max-min)/sliderSteps`, `decimals = ceil(-log10(step))`.
- Tick marks are enabled with interval `max(1, sliderSteps/10)`.
- `setValue()` dirty-checks against `m_v`, which mirrors **what the controls currently show - user edits included** (both sync lambdas write it). A shadow copy that tracked only programmatic writes would go stale the moment the user touched the control, and a later push of that same value would then be silently dropped while the widget still displayed the user's number - i.e. a panel could never resynchronise the widget after an undo.
- `pressed()` / `released()` bracket the whole mouse gesture. They come from an event filter on the slider, not from `sliderPressed` / `sliderReleased`, which miss groove clicks and would split a click+drag into two undo entries.

## Vec3Spin

`Vec3Spin` (`src/ui/items/Vec3Spin.h`) is a reusable composite `QWidget`:

- Three `QDoubleSpinBox` widgets in a horizontal row (X, Y, Z).
- Configurable range, step, decimals and suffix.
- Emits a `valueChanged(x, y, z)` signal when any spinbox changes.
- `setValue(x, y, z)` with an internal dirty-check - no-ops if all three values are unchanged.
- Uses `QSignalBlocker` internally to prevent feedback loops during programmatic updates.

## ColorButton

`ColorButton` (`src/ui/items/ColorButton.h`) is a flat `QPushButton` painted with the current color; clicking it opens the color dialog and, on accept, emits `colorChanged()` once. It is the only color-editing primitive in the UI layer. The swatch is a stylesheet background, so it follows the widget's own enabled/disabled rendering, and an optional alpha channel serves debug tints (vec4) versus light colors (vec3).

- `setColor()` updates the shown color without emitting, so a programmatic `Refresh()` push never looks like a user edit.
- **The dialog is opened asynchronously - do not use `QColorDialog::getColor()`.** `getColor()` shows the dialog through `QDialog::exec()`, a *nested modal event loop*. Inside it this app's render timer keeps re-entering `Application::newFrame()` (edit -> draw -> `Refresh()`), and on Windows (where `QColorDialog` is Qt's own widget dialog, not the native one) the loop also returns keyboard focus to the viewport when it closes - so the swatch looked like it never opened the picker. `ColorButton` keeps one reusable `QColorDialog` and shows it with `open()` (window-modal, asynchronous), applying the result through its selected handler - no nested loop, no focus theft. The same reasoning applies to any future modal dialog in the UI layer.

## CodeEditor

`CodeEditor` (`src/ui/items/CodeEditor.h`) is a `QPlainTextEdit` subclass for GLSL code with optional line numbers (a `LineNumberArea` child widget) and a monospace font:

- `setLanguage(Language)` wires up a `CodeHighlighter` for syntax coloring.
- `setCode()` / `getCode()` exchange the buffer as a `std::string`.
- `setLineNumbersVisible()` toggles the gutter.
- `codeChanged(const std::string&)` fires on edits; `editingStarted()` / `editingFinished()` bracket the focus gesture so a code edit maps to one undo entry.

See `src/ui/utils/CodeHighlighter.h` for the GLSL highlighter.

## OutlinerRow

`OutlinerRow` (`src/ui/items/OutlinerRow.h`) is a single Blender-style outliner row: `[type icon] [name] [eye toggle] [monitor toggle]`, 28px tall. It is pool-friendly and self-contained with no scene-layer dependencies - the object's integer UID is stored directly and surfaced through Qt signals.

- The constructor creates the layout and child widgets once; `setObject(icon, name, objectUid)` binds data, resets toggles to checked (signals blocked) and sets their icons. The stored UID is dirty-checked, so a row already bound to the same object is left untouched.
- `setVisibilities(viewportVisible, renderVisible)` applies toggle states without emitting.
- `setSelectionMode(SelectionMode)` maps the selection mode to a `selectionState` dynamic property on the name button (Active -> `"active"`, Selected -> `"selected"`, neither -> `"normal"`), which QSS colors.
- `setRowIndex(int)` applies alternating background (odd rows translucent white, even transparent), dirty-checked.
- Signal lambdas read `m_objectUid` at emission time, so recycling a row to a new object is transparent - no manual rewire needed. Emits `objectClicked(const ObjectClicked&)` and `visibilityChanged(const VisibilityChanged&)`.
- `retranslate(viewportTip, renderTip)` re-applies the two toggle tooltips; the tooltips are set once per row and never rewritten by `Refresh()`, so a language switch has to reach every pooled row.

## ProfilingRow

`ProfilingRow` and `ProfilingHead` (`src/ui/items/ProfilingRow.h`) are dirty-checked `QTreeWidgetItem` wrappers for the profiling tree:

- `ProfilingRow` is one per-pass child row (columns: Pass, CPU ms, GPU ms, Draws, Dispatches). `setPass(...)` dirty-checks each column independently - timing columns compare on centi-ms (2-decimal) resolution so a sub-0.005ms EMA drift does not trigger a write, and the name column compares the raw `std::string`. `setHidden()` no-ops when unchanged. The wrapped item is created once and recycled across frames.
- `ProfilingHead` is the bold top-level "Frame" totals row with two modes: `setFrame(...)` shows numeric totals, `setNoData()` shows the "No profiling data yet" placeholder and clears the columns. `retranslate(frameLabel, noDataLabel)` stores both column-0 labels and re-applies the current mode's one - the labels are kept rather than only written, because the two modes rewrite column 0 on every transition and a literal there would undo the translation on the next flip.

## ShaderFieldRow

`ShaderFieldRow` (`src/ui/items/ShaderFieldRow.h`) is a reusable widget for a single shader field: a `QComboBox` for the GLSL type and a `QLineEdit` for the name. `setField(type, name)` is dirty-checked against cached values, `setReadOnly()` makes the widgets read-only, and `resetCaches()` clears the cache sentinels when the field identity changes. It emits `fieldChanged(type, name)` on a user edit.

The structure editor itself is a `QTreeView` driven by `ShaderStructModel` and `ShaderFieldDelegate` - see [Models](models.md).
