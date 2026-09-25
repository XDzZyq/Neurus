/**
 * @file DebugProperties.cpp
 * @brief DebugProperties implementation.
 */

#include "presets/DebugProperties.h"

#include "items/ColorButton.h"
#include "items/ScalarSlider.h"
#include "ui/utils/I18n.h"

#include "scene/ObjectID.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QModelIndex>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace neurus
{

namespace
{

/// @brief Column order of the position table.
enum Column
{
	kColX = 0,
	kColY = 1,
	kColZ = 2,
	kColCount = 3,
};

glm::vec4 ToVec4(const QColor& c)
{
	return glm::vec4(static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
	                 static_cast<float>(c.blueF()), static_cast<float>(c.alphaF()));
}

QColor ToQColor(const glm::vec4& c)
{
	QColor out;
	out.setRgbF(glm::clamp(c.r, 0.0f, 1.0f), glm::clamp(c.g, 0.0f, 1.0f),
	            glm::clamp(c.b, 0.0f, 1.0f), glm::clamp(c.a, 0.0f, 1.0f));
	return out;
}

} // namespace

// =========================================================================
// Constructor
// =========================================================================

DebugProperties::DebugProperties(QWidget* parent)
	: QWidget(parent)
{
	auto* outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	m_group = new QGroupBox("Debug", this);
	outerLayout->addWidget(m_group);

	auto* groupLayout = new QVBoxLayout(m_group);
	groupLayout->setSpacing(6);

	BuildSharedRows(groupLayout);
	BuildLineRows(groupLayout);
	BuildPointRows(groupLayout);
	BuildPositionTable(groupLayout);

	outerLayout->addStretch();

	// Nothing is bound yet, so start with every type-specific row hidden; the
	// first setDebugType() reveals the right set.
	setDebugType(static_cast<int>(ObjectID::GOType::NONE_GO));

	Retranslate();
	ConnectSignals();
}

void DebugProperties::BuildSharedRows(QVBoxLayout* layout)
{
	// --- Row: Color swatch ---
	auto* colorRow = new QHBoxLayout();
	m_colorLabel = new QLabel("Color");
	colorRow->addWidget(m_colorLabel);
	m_colorBtn = new ColorButton(/*withAlpha=*/true);
	colorRow->addWidget(m_colorBtn, 1);
	layout->addLayout(colorRow);

	// --- Row: Opacity ---
	auto* opacityRow = new QHBoxLayout();
	m_opacityLabel = new QLabel("Opacity");
	opacityRow->addWidget(m_opacityLabel);
	m_opacitySlider = new ScalarSlider(0.0, 1.0, 100, 1.0);
	opacityRow->addWidget(m_opacitySlider, 1);
	layout->addLayout(opacityRow);

	// --- Row: X-Ray ---
	m_xrayChk = new QCheckBox("X-Ray (see through geometry)");
	layout->addWidget(m_xrayChk);
}

void DebugProperties::BuildLineRows(QVBoxLayout* layout)
{
	// --- Row: Width (pixels) ---
	m_widthRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_widthRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_widthLabel = new QLabel("Width");
		row->addWidget(m_widthLabel);
		m_widthSlider = new ScalarSlider(0.5, 20.0, 195, 1.0);
		row->addWidget(m_widthSlider, 1);
	}
	layout->addWidget(m_widthRow);

	m_stippleChk = new QCheckBox("Stipple");
	layout->addWidget(m_stippleChk);
}

void DebugProperties::BuildPointRows(QVBoxLayout* layout)
{
	// --- Row: Point type (order matches DebugPoints::PointType) ---
	m_pointTypeRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_pointTypeRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_pointTypeLabel = new QLabel("Shape");
		row->addWidget(m_pointTypeLabel);
		m_pointTypeCombo = new QComboBox();
		m_pointTypeCombo->addItems({ "Square", "Rhombus", "Circle", "Cube" });
		row->addWidget(m_pointTypeCombo, 1);
	}
	layout->addWidget(m_pointTypeRow);

	// --- Row: Size (pixels or world units, per the projection mode) ---
	m_scaleRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_scaleRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_scaleLabel = new QLabel("Size");
		row->addWidget(m_scaleLabel);
		m_scaleSlider = new ScalarSlider(0.5, 64.0, 127, 8.0);
		row->addWidget(m_scaleSlider, 1);
	}
	layout->addWidget(m_scaleRow);

	// --- Row: Projection mode (0 = screen space, 1 = world space) ---
	m_projectionRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_projectionRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_projectionLabel = new QLabel("Size Mode");
		row->addWidget(m_projectionLabel);
		m_projectionCombo = new QComboBox();
		m_projectionCombo->addItems({ "Screen Space (px)", "World Space" });
		row->addWidget(m_projectionCombo, 1);
	}
	layout->addWidget(m_projectionRow);

	// --- Row: source mesh path (DebugMesh, read-only) ---
	m_meshRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_meshRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_meshCaption = new QLabel("Source");
		row->addWidget(m_meshCaption);
		m_meshPathLabel = new QLabel("-");
		m_meshPathLabel->setWordWrap(true);
		m_meshPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		row->addWidget(m_meshPathLabel, 1);
	}
	layout->addWidget(m_meshRow);
}

void DebugProperties::BuildPositionTable(QVBoxLayout* layout)
{
	m_positionsBlock = new QWidget();
	auto* blockLayout = new QVBoxLayout(m_positionsBlock);
	blockLayout->setContentsMargins(0, 0, 0, 0);
	blockLayout->setSpacing(4);

	m_positionsLabel = new QLabel("Positions");
	blockLayout->addWidget(m_positionsLabel);

	m_positionsTable = new QTableWidget(0, kColCount);
	m_positionsTable->setHorizontalHeaderLabels({ "X", "Y", "Z" });
	m_positionsTable->verticalHeader()->setDefaultSectionSize(20);
	m_positionsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_positionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_positionsTable->setEditTriggers(QAbstractItemView::DoubleClicked |
	                                  QAbstractItemView::SelectedClicked |
	                                  QAbstractItemView::EditKeyPressed);
	// Tall enough to work in, short enough to leave the shared rows visible.
	m_positionsTable->setMinimumHeight(120);
	m_positionsTable->setMaximumHeight(220);
	blockLayout->addWidget(m_positionsTable);

	auto* buttonRow = new QHBoxLayout();
	m_addBtn = new QPushButton("Add");
	m_removeBtn = new QPushButton("Remove");
	buttonRow->addWidget(m_addBtn);
	buttonRow->addWidget(m_removeBtn);
	buttonRow->addStretch();
	m_countLabel = new QLabel("0");
	buttonRow->addWidget(m_countLabel);
	blockLayout->addLayout(buttonRow);

	layout->addWidget(m_positionsBlock);
}

// =========================================================================
// Signal wiring
// =========================================================================

void DebugProperties::ConnectSignals()
{
	// Every emit is guarded by a bound object: the panel exists before anything
	// is selected, and a stray edit with m_objectId < 0 would reach no handler
	// but would still be recorded as a user gesture.
	QObject::connect(m_colorBtn, &ColorButton::colorChanged, this,
		[this](const QColor& c) {
			if (m_objectId < 0) return;
			const glm::vec4 v = ToVec4(c);
			m_cachedColor = v;
			emit colorChanged(m_objectId, v);
		});

	QObject::connect(m_opacitySlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit opacityChanged(m_objectId, static_cast<float>(m_opacitySlider->value()));
		});

	QObject::connect(m_xrayChk, &QCheckBox::toggled, this,
		[this](bool checked) {
			if (m_objectId < 0) return;
			emit xrayChanged(m_objectId, checked);
		});

	QObject::connect(m_widthSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit lineWidthChanged(m_objectId, static_cast<float>(m_widthSlider->value()));
		});

	QObject::connect(m_stippleChk, &QCheckBox::toggled, this,
		[this](bool checked) {
			if (m_objectId < 0) return;
			emit stippleChanged(m_objectId, checked);
		});

	QObject::connect(m_pointTypeCombo, &QComboBox::currentIndexChanged, this,
		[this](int index) {
			if (m_objectId < 0 || index < 0) return;
			emit pointTypeChanged(m_objectId, index);
		});

	QObject::connect(m_scaleSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit pointScaleChanged(m_objectId, static_cast<float>(m_scaleSlider->value()));
		});

	QObject::connect(m_projectionCombo, &QComboBox::currentIndexChanged, this,
		[this](int index) {
			if (m_objectId < 0 || index < 0) return;
			emit projectionModeChanged(m_objectId, index);
		});

	QObject::connect(m_positionsTable, &QTableWidget::itemChanged, this,
		[this](QTableWidgetItem*) { EmitPositions(); });

	QObject::connect(m_addBtn, &QPushButton::clicked, this, [this]() { AddPosition(); });
	QObject::connect(m_removeBtn, &QPushButton::clicked, this, [this]() { RemovePosition(); });
}

// =========================================================================
// Type switching + translation
// =========================================================================

void DebugProperties::setDebugType(int goType)
{
	const bool isLine = goType == static_cast<int>(ObjectID::GOType::GO_DL);
	const bool isPoints = goType == static_cast<int>(ObjectID::GOType::GO_DP);
	const bool isMesh = goType == static_cast<int>(ObjectID::GOType::GO_DM);

	m_widthRow->setVisible(isLine);
	m_stippleChk->setVisible(isLine);

	m_pointTypeRow->setVisible(isPoints);
	m_scaleRow->setVisible(isPoints);
	m_projectionRow->setVisible(isPoints);

	m_meshRow->setVisible(isMesh);

	// A DebugMesh's geometry comes from a pooled MeshData, so it has no editable
	// position list — the table belongs to the two hand-authored types only.
	m_positionsBlock->setVisible(isLine || isPoints);
}

void DebugProperties::Retranslate()
{
	auto& i18n = I18n::instance();
	m_group->setTitle(i18n.translate("Debug"));
	m_colorLabel->setText(i18n.translate("Color"));
	m_opacityLabel->setText(i18n.translate("Opacity"));
	m_xrayChk->setText(i18n.translate("X-Ray (see through geometry)"));
	m_widthLabel->setText(i18n.translate("Width"));
	m_stippleChk->setText(i18n.translate("Stipple"));
	m_pointTypeLabel->setText(i18n.translate("Shape"));
	m_scaleLabel->setText(i18n.translate("Size"));
	m_projectionLabel->setText(i18n.translate("Size Mode"));
	m_meshCaption->setText(i18n.translate("Source"));
	m_positionsLabel->setText(i18n.translate("Positions"));
	m_addBtn->setText(i18n.translate("Add"));
	m_removeBtn->setText(i18n.translate("Remove"));

	// Combo items are re-set by index so the current selection survives; the
	// signals are blocked because a programmatic rewrite is not a user edit.
	const int typeIndex = m_pointTypeCombo->currentIndex();
	m_pointTypeCombo->blockSignals(true);
	m_pointTypeCombo->setItemText(0, i18n.translate("Square"));
	m_pointTypeCombo->setItemText(1, i18n.translate("Rhombus"));
	m_pointTypeCombo->setItemText(2, i18n.translate("Circle"));
	m_pointTypeCombo->setItemText(3, i18n.translate("Cube"));
	m_pointTypeCombo->setCurrentIndex(typeIndex);
	m_pointTypeCombo->blockSignals(false);

	const int modeIndex = m_projectionCombo->currentIndex();
	m_projectionCombo->blockSignals(true);
	m_projectionCombo->setItemText(0, i18n.translate("Screen Space (px)"));
	m_projectionCombo->setItemText(1, i18n.translate("World Space"));
	m_projectionCombo->setCurrentIndex(modeIndex);
	m_projectionCombo->blockSignals(false);

	m_positionsTable->setHorizontalHeaderLabels(
		{ i18n.translate("X"), i18n.translate("Y"), i18n.translate("Z") });
}

// =========================================================================
// Setters (all dirty-checked)
// =========================================================================

void DebugProperties::setObjectId(int id)
{
	if (m_objectId == id)
		return;
	m_objectId = id;
	m_cachedColor = glm::vec4(-1.0f);
	m_cachedOpacity = -1.0f;
	m_cachedXRay = -1;
	m_cachedWidth = -1.0f;
	m_cachedStipple = -1;
	m_cachedPointType = -1;
	m_cachedScale = -1.0f;
	m_cachedProjection = -1;
	m_cachedMeshPath.clear();
	m_cachedPositions.clear();
}

void DebugProperties::setColor(const glm::vec4& color)
{
	if (m_cachedColor == color) return;
	m_cachedColor = color;
	m_colorBtn->setColor(ToQColor(color));
}

void DebugProperties::setOpacity(float opacity)
{
	if (m_cachedOpacity == opacity) return;
	m_cachedOpacity = opacity;
	m_opacitySlider->setValue(static_cast<double>(opacity));
}

void DebugProperties::setXRay(bool xray)
{
	const int val = xray ? 1 : 0;
	if (m_cachedXRay == val) return;
	m_cachedXRay = val;
	m_xrayChk->blockSignals(true);
	m_xrayChk->setChecked(xray);
	m_xrayChk->blockSignals(false);
}

void DebugProperties::setLineWidth(float width)
{
	if (m_cachedWidth == width) return;
	m_cachedWidth = width;
	m_widthSlider->setValue(static_cast<double>(width));
}

void DebugProperties::setStipple(bool stipple)
{
	const int val = stipple ? 1 : 0;
	if (m_cachedStipple == val) return;
	m_cachedStipple = val;
	m_stippleChk->blockSignals(true);
	m_stippleChk->setChecked(stipple);
	m_stippleChk->blockSignals(false);
}

void DebugProperties::setPointType(int pointType)
{
	if (m_cachedPointType == pointType) return;
	m_cachedPointType = pointType;
	m_pointTypeCombo->blockSignals(true);
	m_pointTypeCombo->setCurrentIndex(pointType);
	m_pointTypeCombo->blockSignals(false);
}

void DebugProperties::setPointScale(float scale)
{
	if (m_cachedScale == scale) return;
	m_cachedScale = scale;
	m_scaleSlider->setValue(static_cast<double>(scale));
}

void DebugProperties::setProjectionMode(int mode)
{
	if (m_cachedProjection == mode) return;
	m_cachedProjection = mode;
	m_projectionCombo->blockSignals(true);
	m_projectionCombo->setCurrentIndex(mode);
	m_projectionCombo->blockSignals(false);
}

void DebugProperties::setMeshPath(const QString& path)
{
	if (m_cachedMeshPath == path) return;
	m_cachedMeshPath = path;
	m_meshPathLabel->setText(path.isEmpty() ? "-" : path);
	m_meshPathLabel->setToolTip(path);
}

// =========================================================================
// Position table
// =========================================================================

void DebugProperties::setPositions(const std::vector<glm::vec3>& positions)
{
	if (m_cachedPositions == positions)
		return;  // the common case: Refresh() pushing back what we just emitted
	m_cachedPositions = positions;

	const int total = static_cast<int>(positions.size());
	const int shown = std::min(total, kMaxRows);
	const bool truncated = total > shown;

	// itemChanged fires per cell while populating; without this guard each write
	// would read the half-built table back out and emit a bogus list.
	m_populating = true;
	m_positionsTable->setRowCount(shown);
	for (int row = 0; row < shown; ++row)
	{
		const glm::vec3& p = positions[static_cast<size_t>(row)];
		const float xyz[kColCount] = { p.x, p.y, p.z };
		for (int col = 0; col < kColCount; ++col)
		{
			QTableWidgetItem* item = m_positionsTable->item(row, col);
			if (!item)
			{
				item = new QTableWidgetItem();
				m_positionsTable->setItem(row, col, item);
			}
			item->setText(QString::number(static_cast<double>(xyz[col]), 'f', 3));
		}
	}
	m_populating = false;

	auto& i18n = I18n::instance();
	if (truncated)
	{
		// Read-only rather than partially editable: an edit here would write back
		// only the visible rows and silently drop the hidden tail.
		m_positionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
		m_countLabel->setText(QString("%1 / %2 %3")
		                          .arg(shown)
		                          .arg(total)
		                          .arg(i18n.translate("(read-only)")));
	}
	else
	{
		m_positionsTable->setEditTriggers(QAbstractItemView::DoubleClicked |
		                                  QAbstractItemView::SelectedClicked |
		                                  QAbstractItemView::EditKeyPressed);
		m_countLabel->setText(QString::number(total));
	}
	m_addBtn->setEnabled(!truncated);
	m_removeBtn->setEnabled(!truncated && total > 0);
}

void DebugProperties::EmitPositions()
{
	if (m_populating || m_objectId < 0)
		return;

	std::vector<glm::vec3> out;
	const int rows = m_positionsTable->rowCount();
	out.reserve(static_cast<size_t>(rows));
	for (int row = 0; row < rows; ++row)
	{
		float xyz[kColCount] = { 0.0f, 0.0f, 0.0f };
		for (int col = 0; col < kColCount; ++col)
		{
			const QTableWidgetItem* item = m_positionsTable->item(row, col);
			if (!item) continue;
			bool ok = false;
			const double v = item->text().toDouble(&ok);
			// Unparseable text reads as the coordinate it replaced, so a typo
			// cannot teleport a vertex to the origin mid-edit.
			if (ok)
				xyz[col] = static_cast<float>(v);
			else if (row < static_cast<int>(m_cachedPositions.size()))
				xyz[col] = m_cachedPositions[static_cast<size_t>(row)][col];
		}
		out.emplace_back(xyz[kColX], xyz[kColY], xyz[kColZ]);
	}

	if (out == m_cachedPositions)
		return;
	m_cachedPositions = out;
	m_countLabel->setText(QString::number(out.size()));
	emit positionsChanged(m_objectId, out);
}

void DebugProperties::AddPosition()
{
	if (m_objectId < 0 || static_cast<int>(m_cachedPositions.size()) >= kMaxRows)
		return;

	// Seeded from the last entry so appending to a line continues from where it
	// ended instead of dropping a degenerate segment at the origin.
	std::vector<glm::vec3> next = m_cachedPositions;
	next.push_back(next.empty() ? glm::vec3(0.0f) : next.back());

	// The cache is deliberately left stale: the table is repopulated by the
	// Refresh() that follows the scene mutation, and setPositions() only rebuilds
	// when the pushed list differs from the cache.
	emit positionsChanged(m_objectId, next);
}

void DebugProperties::RemovePosition()
{
	if (m_objectId < 0 || m_cachedPositions.empty())
		return;

	// Selected rows, or the last entry when nothing is selected.
	std::vector<bool> drop(m_cachedPositions.size(), false);
	bool any = false;
	for (const QModelIndex& index : m_positionsTable->selectionModel()->selectedRows())
	{
		const int row = index.row();
		if (row >= 0 && row < static_cast<int>(drop.size()))
		{
			drop[static_cast<size_t>(row)] = true;
			any = true;
		}
	}
	if (!any)
		drop.back() = true;

	std::vector<glm::vec3> next;
	next.reserve(m_cachedPositions.size());
	for (size_t i = 0; i < m_cachedPositions.size(); ++i)
	{
		if (!drop[i])
			next.push_back(m_cachedPositions[i]);
	}

	// Cache left stale on purpose — see AddPosition().
	emit positionsChanged(m_objectId, next);
}

} // namespace neurus
