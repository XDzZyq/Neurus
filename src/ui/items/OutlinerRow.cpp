/**
 * @file OutlinerRow.cpp
 * @brief OutlinerRow implementation — two-phase config: setObject (data) then setSelectionMode + setRowIndex (visual).
 *
 * Type icons are loaded from the Icons cache as QPixmaps.
 * Visibility toggle buttons swap between visible/invisible icons
 * sourced from the Icons cache on each toggle.
 */

#include "items/OutlinerRow.h"

#include <QFile>
#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTextStream>
#include <QStyle>

#include <QGuiApplication>

#include "Icons.h"
#include "core/Selections.h"
#include "editor/Input.h"


namespace neurus
{

// =========================================================================
// Outliner stylesheet (loaded once from outliner.qss Qt resource)
// =========================================================================

static QString s_outlinerStyle;

static void LoadOutlinerStyle()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	QFile file(":/qml/outliner.qss");
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
	s_outlinerStyle = QTextStream(&file).readAll();
	file.close();
}

// =========================================================================
// Constructor — create layout + child widgets once
// =========================================================================

OutlinerRow::OutlinerRow(QWidget* parent)
	: QWidget(parent)
{
	LoadOutlinerStyle();
	setStyleSheet(s_outlinerStyle);

	setFixedHeight(28);

	auto* rowLayout = new QHBoxLayout(this);
	rowLayout->setContentsMargins(4, 1, 4, 1);
	rowLayout->setSpacing(4);

	// --- Type icon (QLabel with QPixmap, 22x22, centered) ---
	m_typeLabel = new QLabel();
	m_typeLabel->setFixedSize(22, 22);
	m_typeLabel->setAlignment(Qt::AlignCenter);
	rowLayout->addWidget(m_typeLabel);

	// --- Name (flat QPushButton styled as label, clickable for selection) ---
	m_nameBtn = new QPushButton();
	m_nameBtn->setObjectName("outlinerNameBtn");
	m_nameBtn->setFlat(true);
	m_nameBtn->setCursor(Qt::PointingHandCursor);
	m_nameBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	// Lambda reads m_objectUid at emission time — works after setObject
	QObject::connect(m_nameBtn, &QPushButton::clicked, this, [this]() {
		const auto mods = Input::GetModifiers(static_cast<uint32_t>(QGuiApplication::queryKeyboardModifiers().toInt()));
		emit objectClicked(ObjectClicked{m_objectUid, mods});
	});
	rowLayout->addWidget(m_nameBtn);

	// --- Visibility toggle buttons (icon-driven, checkable) ---
	m_eyeBtn    = new QPushButton();
	m_renderBtn = new QPushButton();

	const QSize kToggleBtnSize(26, 26);
	const QSize kToggleIconSize(20, 20);

	// Eye button
	m_eyeBtn->setObjectName("outlinerToggleBtn");
	m_eyeBtn->setCheckable(true);
	m_eyeBtn->setChecked(true);
	m_eyeBtn->setFlat(true);
	m_eyeBtn->setFixedSize(kToggleBtnSize);
	m_eyeBtn->setIconSize(kToggleIconSize);
	m_eyeBtn->setIcon(Icons::GetIconPair("editor:preview_visible", "editor:preview_invisible"));
	m_eyeBtn->setToolTip(QString::fromUtf8("Viewport visibility"));
	m_eyeBtn->setCursor(Qt::PointingHandCursor);
	auto* eyeFx = new QGraphicsColorizeEffect(m_eyeBtn);
	eyeFx->setColor(QColor("#444444"));  // checked = visible = dark
	eyeFx->setEnabled(true);
	m_eyeBtn->setGraphicsEffect(eyeFx);

	// Render button
	m_renderBtn->setObjectName("outlinerToggleBtn");
	m_renderBtn->setCheckable(true);
	m_renderBtn->setChecked(true);
	m_renderBtn->setFlat(true);
	m_renderBtn->setFixedSize(kToggleBtnSize);
	m_renderBtn->setIconSize(kToggleIconSize);
	m_renderBtn->setIcon(Icons::GetIconPair("editor:render_visible", "editor:render_invisible"));
	m_renderBtn->setToolTip(QString::fromUtf8("Render visibility"));
	m_renderBtn->setCursor(Qt::PointingHandCursor);
	auto* renderFx = new QGraphicsColorizeEffect(m_renderBtn);
	renderFx->setColor(QColor("#444444"));  // checked = visible = dark
	renderFx->setEnabled(true);
	m_renderBtn->setGraphicsEffect(renderFx);

	// Connect signals — lambdas read m_objectUid at emission time.
	// Each toggle records its state and applies the colorize tint.
	QObject::connect(m_eyeBtn, &QPushButton::toggled, this,
		[this](bool) {
			m_eyeVisible = m_eyeBtn->isChecked();
			setEyeBtnColor();
			emit visibilityChanged(VisibilityChanged{m_objectUid, m_eyeVisible, m_renderVisible});
		});
	QObject::connect(m_renderBtn, &QPushButton::toggled, this,
		[this](bool) {
			m_renderVisible = m_renderBtn->isChecked();
			setRenderBtnColor();
			emit visibilityChanged(VisibilityChanged{m_objectUid, m_eyeVisible, m_renderVisible});
		});

	rowLayout->addWidget(m_eyeBtn);
	rowLayout->addWidget(m_renderBtn);
}

// =========================================================================
// SetEyeBtnColor — update eye button colorize effect from checked state
// =========================================================================

void OutlinerRow::setEyeBtnColor()
{
	auto* fx = qobject_cast<QGraphicsColorizeEffect*>(m_eyeBtn->graphicsEffect());
	if (fx) fx->setColor(m_eyeBtn->isChecked() ? QColor("#444444") : QColor("#d0d0d0"));
}

// =========================================================================
// SetRenderBtnColor — update render button colorize effect from checked state
// =========================================================================

void OutlinerRow::setRenderBtnColor()
{
	auto* fx = qobject_cast<QGraphicsColorizeEffect*>(m_renderBtn->graphicsEffect());
	if (fx) fx->setColor(m_renderBtn->isChecked() ? QColor("#444444") : QColor("#d0d0d0"));
}

// =========================================================================
// retranslate — re-apply the toggle tooltips (Outliner owns the strings)
// =========================================================================

void OutlinerRow::retranslate(const QString& viewportTip, const QString& renderTip)
{
	m_eyeBtn->setToolTip(viewportTip);
	m_renderBtn->setToolTip(renderTip);
}

// =========================================================================
// setObject — bind object identity data, reset toggles
// =========================================================================

void OutlinerRow::setObject(const QIcon& icon,
                            const QString& name, int objectUid)
{
	// Update name text
	m_nameBtn->setText(name);
	if (m_objectUid == objectUid)
		return;

	m_objectUid = objectUid;

	m_typeLabel->setPixmap(icon.pixmap(22, 22));
	m_typeLabel->setStyleSheet(QString());  // clear old colored-background CSS
}

// =========================================================================
// setVisibilities — set toggle states without emitting signals
// =========================================================================

void OutlinerRow::setVisibilities(bool viewportVisible, bool renderVisible)
{
	if (m_eyeVisible != viewportVisible)
	{
		m_eyeBtn->blockSignals(true);
		m_eyeBtn->setChecked(viewportVisible);
		m_eyeBtn->blockSignals(false);
		m_eyeVisible = viewportVisible;
		setEyeBtnColor();
	}

	if (m_renderVisible != renderVisible)
	{
		m_renderBtn->blockSignals(true);
		m_renderBtn->setChecked(renderVisible);
		m_renderBtn->blockSignals(false);
		m_renderVisible = renderVisible;
		setRenderBtnColor();
	}
}

// =========================================================================
// setSelectionMode — set name button color via QSS dynamic property
// =========================================================================

void OutlinerRow::setSelectionMode(SelectionMode mode)
{
	if (m_mode == static_cast<int>(mode))
		return;

	QString state;
	if (static_cast<int>(mode) & static_cast<int>(SelectionMode::Active))
		state = QStringLiteral("active");
	else if (static_cast<int>(mode) & static_cast<int>(SelectionMode::Selected))
		state = QStringLiteral("selected");
	else
		state = QStringLiteral("normal");

	m_nameBtn->setProperty("selectionState", state);
	m_mode = static_cast<int>(mode);

	// Force QSS re-evaluation for dynamic property change
	m_nameBtn->style()->unpolish(m_nameBtn);
	m_nameBtn->style()->polish(m_nameBtn);
}

// =========================================================================
// setRowIndex — alternating row background
// =========================================================================

void OutlinerRow::setRowIndex(int rowIndex)
{
	if (m_idx == rowIndex)
		return;

	if (rowIndex % 2 == 1)
	{
		QPalette pal = palette();
		pal.setColor(QPalette::Window, QColor(255, 255, 255, 100));
		setPalette(pal);
		setAutoFillBackground(true);
	}
	else
	{
		setAutoFillBackground(false);
	}
	m_idx = rowIndex;
}

} // namespace neurus
