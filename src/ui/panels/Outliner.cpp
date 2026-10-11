/**
 * @file Outliner.cpp
 * @brief Outliner implementation — pool-managed row list from UIContext scene data.
 */

#include "Outliner.h"

#include "Icons.h"
#include "UIContext.h"
#include "ui/utils/I18n.h"
#include "items/OutlinerRow.h"

#include "scene/Scene.h"
#include "scene/ObjectID.h"  // ObjectID, GOType

#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <string>

namespace neurus
{

// =========================================================================
// Constructor — scroll area + empty container (no hard-coded rows)
// =========================================================================

Outliner::Outliner(QWidget* parent)
	: UIPanel(PanelType::Outliner, nullptr, parent)
{
	setFocusPolicy(Qt::StrongFocus);

	auto* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);

	m_scrollArea = new QScrollArea(this);
	m_scrollArea->setWidgetResizable(true);
	m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mainLayout->addWidget(m_scrollArea);

	m_container = new QWidget();
	m_listLayout = new QVBoxLayout(m_container);
	m_listLayout->setContentsMargins(4, 4, 4, 4);
	m_listLayout->setSpacing(2);
	m_listLayout->setAlignment(Qt::AlignTop);

	m_scrollArea->setWidget(m_container);

	m_sceneGroup = AddCategoryGroup(QString::fromUtf8("Scene"));
	m_groupLayout = qobject_cast<QVBoxLayout*>(m_sceneGroup->layout());

	// Apply the active language (group title was built in English).
	Retranslate();
}

// =========================================================================
// Retranslate - re-apply the category-group title + every pooled row's tooltips
// =========================================================================

void Outliner::Retranslate()
{
	m_sceneGroup->setTitle(I18n::instance().translate("Scene"));
	for (OutlinerRow* row : m_rowPool)
		RetranslateRow(row);
}

// =========================================================================
// RetranslateRow - the row texts Refresh() never rewrites (tooltips)
// =========================================================================

void Outliner::RetranslateRow(OutlinerRow* row)
{
	auto& i18n = I18n::instance();
	row->retranslate(i18n.translateCtx("Viewport visibility", "Tooltip"),
	                 i18n.translateCtx("Render visibility", "Tooltip"));
}

// =========================================================================
// AddCategoryGroup — creates a QGroupBox with a QVBoxLayout for rows
// =========================================================================

QGroupBox* Outliner::AddCategoryGroup(const QString& title)
{
	auto* group = new QGroupBox(title);

	auto* groupLayout = new QVBoxLayout(group);
	groupLayout->setContentsMargins(4, 4, 4, 4);
	groupLayout->setSpacing(2);

	m_listLayout->addWidget(group);
	return group;
}

// =========================================================================
// keyPressEvent — Delete = delete all selected objects
// =========================================================================

void Outliner::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Delete)
	{
		// Delete is a pure intent: the request is forwarded, not applied here.
		emit deleteRequested(DeleteRequested{});
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

// =========================================================================
// EnsureRowPool — grow pool to meet needed capacity
// =========================================================================

void Outliner::EnsureRowPool(std::size_t needed)
{
	while (m_rowPool.size() < needed)
	{
		auto* row = new OutlinerRow(m_sceneGroup);

		// Rows can be created after a language switch, so seed the texts that
		// Refresh() never rewrites from the active language, not the ctor's
		// English literals.
		RetranslateRow(row);

		// Connect row signals to Outliner signals once (permanent). Lambdas read
		// m_objectUid at emission time, so recycling a row to a new object works
		// without reconnecting.
		QObject::connect(row, &OutlinerRow::objectClicked,
			this, &Outliner::objectClicked);
		QObject::connect(row, &OutlinerRow::visibilityChanged,
			this, &Outliner::visibilityChanged);

		m_groupLayout->addWidget(row);
		m_rowPool.push_back(row);
	}

	// Hide surplus rows.
	for (std::size_t i = needed; i < m_rowPool.size(); ++i)
	{
		m_rowPool[i]->setVisible(false);
	}
}

// =========================================================================
// Refresh — bind pool rows to scene objects
// =========================================================================

void Outliner::Refresh(const UIContext& ctx)
{
	auto ids = ctx.GetObjectIDs();

	// --- Selection state ---
	const Scene* scene = ctx.snapshot.scene;

	EnsureRowPool(ids.size());

	// Rows hold plain int UIDs: re-binding a row or skipping an unchanged one is
	// a cheap integer comparison, with no cached object pointers anywhere.
	std::size_t poolIndex = 0;
	for (int id : ids)
	{
		const ObjectID* obj = scene ? scene->GetObjectID(id) : nullptr;
		if (!obj) continue; // stale id (should not happen: ids mirror obj_list)

		// Bind object identity data (icon, name, UID).
		m_rowPool[poolIndex]->setObject(
			Icons::ObjectIcon(static_cast<int>(obj->o_type)),
			QString::fromStdString(obj->o_name),
			id);

		m_rowPool[poolIndex]->setVisibilities(obj->is_viewport, obj->is_rendered);

		// Selection highlight (QSS property) + row background.
		neurus::SelectionMode mode = scene->selections.GetMode(obj);
		m_rowPool[poolIndex]->setSelectionMode(mode);
		m_rowPool[poolIndex]->setRowIndex(static_cast<int>(poolIndex));

		m_rowPool[poolIndex]->setVisible(true);
		++poolIndex;
	}

	// Hide the category group when the scene is empty.
	m_sceneGroup->setVisible(!ids.empty());
}

} // namespace neurus
