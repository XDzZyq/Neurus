/**
 * @file Outliner.h
 * @brief Outliner dock panel — Blender-style vertical list view.
 *
 * Displays scene objects as a scrollable vertical list. Each row shows the
 * object type icon, name, and two visibility toggles (viewport / render).
 *
 * Architecture:
 * - UIPanel subclass (matches RenderConfigPanel / PropertyEditor pattern)
 * - QVBoxLayout → QScrollArea → container widget with vertical row list
 * - Rows are OutlinerRow widgets managed via a pool
 * - Pool grows as needed; extra rows are hidden (not destroyed)
 * - Each recycled row calls setObject() — signal lambdas read the current
 *   m_objectUid at emission time, so no manual rewire needed
 * - Row signals forwarded via Outliner::objectClicked / visibilityChanged
 * - Reads scene data via UIContext — no Renderer or Vulkan headers
 */

#pragma once

#include "UIPanel.h"

#include "editor/events/InputEvents.h"
#include "editor/events/SceneEvents.h"

#include <cstddef>
#include <vector>

class QGroupBox;
class QKeyEvent;
class QScrollArea;
class QVBoxLayout;

namespace neurus
{

class OutlinerRow;

class Outliner : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::Outliner;

	explicit Outliner(QWidget* parent = nullptr);
	~Outliner() override = default;

	Outliner(const Outliner&) = delete;
	Outliner& operator=(const Outliner&) = delete;

	/**
	 * @brief Per-frame refresh from UIContext.
	 *
	 * Uses a row pool: grows when scene objects exceed pool size, recycles
	 * existing rows via SetObject() otherwise. Extra rows are hidden.
	 *
	 * @param ctx Read-only UI context carrying Editor/Project state.
	 */
	void Refresh(const UIContext& ctx) override;

	/** @brief Re-applies the category-group title in the active language. */
	void Retranslate() override;

signals:
	/** @brief Emitted when a user clicks on a scene object row in the outliner. */
	void objectClicked(const ObjectClicked& e);

	/** @brief Emitted when visibility toggles change for an object. */
	void visibilityChanged(const VisibilityChanged& e);

	/**
	 * @brief Emitted when the Delete key is pressed while the outliner has focus.
	 * @note Pure intent - the Editor wraps the active scene and deletes ALL selected objects.
	 */
	void deleteRequested(const DeleteRequested& e);

protected:
	/**
	 * @brief Handles keyboard input (Delete = delete all selected objects).
	 * @param event The key event.
	 * @note Key events from focused child rows propagate up to this panel.
	 */
	void keyPressEvent(QKeyEvent* event) override;

private:
	/**
	 * @brief Creates a collapsible QGroupBox for a category of objects.
	 *
	 * @param title Category header text (e.g. "Cameras").
	 * @return The QGroupBox widget.
	 */
	QGroupBox* AddCategoryGroup(const QString& title);

	/**
	 * @brief Ensures the row pool has at least @p needed rows,
	 *        creating new ones as necessary.
	 */
	void EnsureRowPool(std::size_t needed);

	/**
	 * @brief Applies the active language to one pooled row.
	 *
	 * Only covers text Refresh() does not rewrite every frame (the visibility
	 * tooltips); the name and icon come from the scene data.
	 */
	void RetranslateRow(OutlinerRow* row);

	QScrollArea* m_scrollArea = nullptr;
	QWidget*     m_container  = nullptr;
	QVBoxLayout* m_listLayout = nullptr;

	/// Persistent "Scene" category group (created once).
	QGroupBox*     m_sceneGroup = nullptr;
	QVBoxLayout*   m_groupLayout = nullptr;

	/// Row pool — grows as needed, never shrinks.
	std::vector<OutlinerRow*> m_rowPool;
};

} // namespace neurus
