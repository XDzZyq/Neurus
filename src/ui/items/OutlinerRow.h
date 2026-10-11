/**
 * @file OutlinerRow.h
 * @brief Single row widget for the Outliner panel (pool-friendly).
 *
 * Encapsulates a Blender-style outliner row: [type icon] [name] [eye toggle]
 * [monitor toggle]. The object's integer UID is stored directly and surfaced
 * through Qt signals; recycling a row to a different object is transparent.
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
 * Layout: [type icon 22x22] [name button (stretching)] [eye toggle]
 * [monitor toggle], all in a 28px-tall QHBoxLayout.
 */
class OutlinerRow : public QWidget
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs an empty outliner row (no object bound yet).
	 * @param parent Parent widget.
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
	 * Resets the visibility toggles and their icons; the stored UID is
	 * dirty-checked, so a row already bound to the same object is untouched.
	 *
	 * @param icon      Type icon for this object.
	 * @param name      Display name shown in the row.
	 * @param objectUid Scene object UID (0 = none).
	 */
	void setObject(const QIcon& icon, const QString& name, int objectUid);

	/**
	 * @brief Sets visibility toggle states without emitting signals.
	 *
	 * Also swaps the button icons to reflect the new state.
	 *
	 * @param viewportVisible True to enable viewport visibility.
	 * @param renderVisible   True to enable render visibility.
	 */
	void setVisibilities(bool viewportVisible, bool renderVisible);

	/**
	 * @brief Sets the selection visual state via a QSS dynamic property on the
	 *        name button: active -> "active", selected -> "selected", else
	 *        "normal".
	 *
	 * @param mode Selection mode bitmask (Active / Selected).
	 */
	void setSelectionMode(SelectionMode mode);

	/**
	 * @brief Sets row index for alternating background.
	 *
	 * Odd rows get a translucent white background, even rows are transparent.
	 *
	 * @param rowIndex 0-based row position.
	 */
	void setRowIndex(int rowIndex);

	/**
	 * @brief Re-applies the two visibility-toggle tooltips.
	 *
	 * Tooltips are set once per row and never rewritten by Refresh(), so a
	 * language switch has to reach every pooled row.
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
