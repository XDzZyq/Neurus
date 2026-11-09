# UI Models and Delegates

Qt model/view helpers live in `src/ui/models/` (`QAbstractItemModel` subclasses) and `src/ui/delegates/` (`QStyledItemDelegate` subclasses); they are non-widget and reused across panels.

## ShaderStructModel

`ShaderStructModel` (`src/ui/models/ShaderStructModel.h`) is a `QAbstractItemModel` tree for the `ShaderStruct` IR, with a 3-level structure: section headers -> field/struct nodes -> struct members.

- The model is read-only; `setData()` edits emit a `fieldEdited(section, fieldIndex, subFieldIndex, field, value)` signal so the editor controller can mutate the underlying `ShaderStruct` and refresh.
- `setShaderStruct(const ShaderStruct*)` rebuilds the entire tree.
- Custom roles (`RoleNodeType`, `RoleSection`, `RoleFieldIndex`, `RoleSubFieldIndex`) let the delegate address a cell without parsing a display string.
- **Retranslation is on demand.** `headerData()` and `sectionTitle()` call `I18n` when asked, and `Retranslate()` only re-emits (`headerDataChanged` + `dataChanged` over the section rows) - no rebuild, since nothing is cached. Section nodes store their `ShaderSection`, never a title string. The owning `ShaderEditorPanel` calls it from its own `Retranslate()`.

Used by the `ShaderEditorPanel` structure mode. See [Panels](panels.md#shadereditorpanel).

## LogModel

`LogModel` (`src/ui/models/LogModel.h`) is a read-only `QAbstractListModel` over the core `LogBuffer`, feeding the virtualized `QListView` in the `LogPanel`. Row `r` is the entry with seq `firstSeq + r`, read through `LogBuffer::BySeq`. `Refresh(buffer)` takes one `Snapshot()` and resyncs the model:

- Entries the ring evicted -> `beginRemoveRows` from the front, preserving scroll position.
- New entries -> `beginInsertRows` at the end.
- Sequence drop (Clear) or first attach -> `beginResetModel`.

`rowCount()` returns the model's own count, never the live buffer size: the render thread appends at any time, and a count that moves outside the begin/end notifications desynchronises `QSortFilterProxyModel`'s mapping (it crashed on the next removal).

Qt only calls `data()` for visible rows, so a 10k-entry buffer costs roughly 50 row reads per frame. Custom roles (`LevelRole`, `TimestampRole`, `SourceRole`, `MessageRole`, `SeqRole`) let the delegate paint severity color and the proxy filter by level/message without parsing a combined display string. `MaxSourceChars()` reports the character width of the longest `func:line` source field, which the delegate uses to align message text; `FormatLogTimestamp()` formats a `system_clock` time point as `HH:MM:SS.mmm` local time.

## LogFilterProxy

`LogFilterProxy` (`src/ui/models/LogFilterProxy.h`) is a `QSortFilterProxyModel` between `LogModel` and the `LogPanel`'s `QListView`:

- `setLevelFilter(LevelFilter)` selects `All`, `InfoOnly` or `ErrorsOnly`.
- `setSearchText(QString)` sets a case-insensitive substring search (empty = off).
- Only `filterAcceptsRow` is overridden; dynamic sorting stays off, so source order (chronological) is preserved.

## ShaderFieldDelegate

`ShaderFieldDelegate` (`src/ui/delegates/ShaderFieldDelegate.h`) is a `QStyledItemDelegate` for editing shader struct fields in the tree:

- `createEditor()` renders a `QComboBox` for type columns and a `QLineEdit` for name columns; section headers are not editable.
- Section header and struct-definition rows are first-column spanned; the delegate paints a "+" button at the right edge of that spanned cell and emits `addClicked(index)` on a left click there - a plain-delegate control, so the tree keeps **no** per-row index widgets (those fought with the frequent model resets and macOS accessibility, causing input loss after clicking "+").
- **Delegate editors are created on demand, so they translate at `createEditor()` time and need no `Retranslate()` hook** (for example the `"struct name"` placeholder).

## LogDelegate

`LogDelegate` (`src/ui/delegates/LogDelegate.h`) is the `QStyledItemDelegate` for the `LogPanel`'s `QListView`. It paints each row as `HH:MM:SS.mmm  [func:line]  message` in a fixed-width font, colored by severity (red `#d32f2f` for errors, dark gray `#303030` for info) while keeping selection and hover backgrounds. `setSourcePad(chars)` sets the monospace column width used to right-justify the `[func:line]` source field so message text starts at the same X on every row.

## Retranslation

Item models that produce display text translate **on demand** through `I18n` and expose a `Retranslate()` that re-emits `headerDataChanged` / `dataChanged` rather than rebuilding (`ShaderStructModel`). Because nothing is cached, no rebuild is needed on a language switch. Delegate editors are created on demand, so they translate at `createEditor()` time and need no hook at all. See [i18n](i18n.md).
