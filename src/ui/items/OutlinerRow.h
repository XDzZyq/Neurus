/**
 * @file OutlinerRow.h
 * @brief Single row widget for the Outliner panel (pool-friendly).
 *
 * OutlinerRow encapsulates a Blender-style outliner row: [type icon] [name]
 * [eye toggle] [monitor toggle]. It is self-contained with no scene-layer
 * dependencies — the object's integer UID is stored directly and surfaced
 * through Qt signals.
 *
 * Architecture:
 * - Constructor creates the layout and child widgets once.
 * - setObject() binds object data (icon name, name, object UID); resets toggles.
 * - setSelectionMode() / setRowIndex() apply visual state (QSS property + alternating bg).
 * - Signal lambdas read from m_objectUid at emission time, so recycling
 *   a row to a new object is transparent — no manual rewire needed.
 * - Visibility toggle buttons swap between visible/invisible icons
 *   loaded via Icons::GetIcon().
 */

#pragma once

#include <QIcon>
#include <QWidget>

#include "editor/events/InputEvents.h"
#include "editor/events/SceneEvents.h"
#include "scene/ObjectID.h"

#include <cstdint>

#include <string>

class QLabel;
class QPushButton;

namespace neurus
{

/** @brief Forward declaration (defined in core/Selections.h). */
enum class SelectionMode : uint32_t;

/**
 * @brief A single row in the Outliner list view (pool-compatible).
 *
 * Layout (all in QHBoxLayout, 28px height):
 *   [type icon 22x22] [name QPushButton (stretching)] [eye toggle] [monitor toggle]
 */
class OutlinerRow : public QWidget
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs an empty outliner row (no object bound yet).
	 * @param parent Parent widget.
	 *
	 * Creates the layout and child widgets once. Call setObject() to
	 * bind a scene object to this row.
	 */
	explicit OutlinerRow(QWidget* parent = nullptr);
	~OutlinerRow() override = default;

	OutlinerRow(const OutlinerRow&) = delete;
	OutlinerRow& operator=(const OutlinerRow&) = delete;

	// -------------------------------------------------------------------
	// Two-phase row configuration: bind data, then apply style
	// -------------------------------------------------------------------

	/**
	 * @brief Binds a scene object's identity data to this row.
	 *
	 * Updates the type icon pixmap, name text, and stored object UID.
	 * Resets visibility toggles to checked (signals blocked to avoid
	 * cascading events during pool recycling) and sets their icons.
	 * Dirty-checks the stored UID: a row already bound to the same object
	 * is left untouched (lazy update).
	 *
	 * @param icon      Type icon for this object.
	 * @param name      Display name shown in the row.
	 * @param objectUid Scene object UID (0 = none).
	 */
	void setObject(const QIcon& icon, const QString& name, int objectUid);

	/**
	 * @brief Sets visibility toggle states without emitting signals.
	 *
	 * Updates both the viewport (eye) and render (monitor) toggle buttons
	 * using blockSignals so no false visibilityChanged emissions occur.
	 * Also swaps the button icons to reflect the new state.
	 *
	 * @param viewportVisible True to enable viewport visibility.
	 * @param renderVisible   True to enable render visibility.
	 */
	void setVisibilities(bool viewportVisible, bool renderVisible);

	/**
	 * @brief Sets selection visual state via QSS dynamic property.
	 *
	 * Maps SelectionMode to "selectionState" property on m_nameBtn:
	 *   Active   → "active"   (#ff6f00 via QSS)
	 *   Selected → "selected" (#4A90D9 via QSS)
	 *   Neither  → "normal"   (#000000 via QSS)
	 *
	 * @param mode   Selection mode bitmask (Active / Selected).
	 */
	void setSelectionMode(SelectionMode mode);

	/**
	 * @brief Sets row index for alternating background.
	 *
	 * Odd rows get a translucent white background, even rows are transparent.
	 * No-op if rowIndex hasn't changed (dirty check).
	 *
	 * @param rowIndex  0-based row position.
	 */
	void setRowIndex(int rowIndex);

	/**
	 * @brief Re-applies the two visibility-toggle tooltips.
	 *
	 * The tooltips are set once per row and never rewritten by Refresh(), so a
	 * language switch has to reach every pooled row — Outliner::Retranslate()
	 * walks the pool, and EnsureRowPool() calls this on each new row so rows
	 * created after the switch are not left in English.
	 *
	 * @param viewportTip Translated tooltip for the preview-visibility toggle.
	 * @param renderTip   Translated tooltip for the render-visibility toggle.
	 */
	void retranslate(const QString& viewportTip, const QString& renderTip);

	/** @brief Returns the bound object UID (0 if none). */
	int getObjectUid() const { return m_objectUid; }

signals:
	/** @brief Emitted when the name button is clicked. */
	void objectClicked(const ObjectClicked& e);

	/** @brief Emitted when either visibility toggle changes. */
	void visibilityChanged(const VisibilityChanged& e);

private:
	/** @brief Sets the eye button colorize effect from current checked state. */
	void setEyeBtnColor();

	/** @brief Sets the render button colorize effect from current checked state. */
	void setRenderBtnColor();

	QLabel*      m_typeLabel     = nullptr;
	QPushButton* m_nameBtn       = nullptr;
	QPushButton* m_eyeBtn        = nullptr;
	QPushButton* m_renderBtn     = nullptr;
	int          m_objectUid     = 0;
	int 		 m_mode          = -1;
	int 		 m_idx           = -1;
	bool         m_eyeVisible    = true;
	bool         m_renderVisible = true;
};

} // namespace neurus
