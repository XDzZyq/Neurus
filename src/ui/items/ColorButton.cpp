/**
 * @file ColorButton.cpp
 * @brief ColorButton implementation.
 */

#include "items/ColorButton.h"

#include <QColorDialog>

namespace neurus
{

ColorButton::ColorButton(bool withAlpha, QWidget* parent)
	: QPushButton(parent)
	, m_withAlpha(withAlpha)
{
	setFlat(true);
	setAutoFillBackground(true);
	setFixedHeight(20);
	setMinimumWidth(48);
	setCursor(Qt::PointingHandCursor);
	UpdateSwatch();

	QObject::connect(this, &QPushButton::clicked, this, &ColorButton::PickColor);
}

void ColorButton::setColor(const QColor& c)
{
	if (c == m_color)
		return;
	m_color = c;
	UpdateSwatch();
}

void ColorButton::UpdateSwatch()
{
	// The alpha is shown as a solid blend against the panel rather than a
	// checkerboard: a debug tint is drawn over the viewport, so the swatch is
	// only ever a hint at the hue, not a preview of the composite.
	const QColor opaque(m_color.red(), m_color.green(), m_color.blue());
	const QString border = opaque.lightnessF() > 0.5 ? "#404040" : "#a0a0a0";
	setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid %2; "
	                      "border-radius: 2px; }")
	                  .arg(opaque.name(), border));
	setToolTip(m_withAlpha ? m_color.name(QColor::HexArgb) : opaque.name());
}

void ColorButton::PickColor()
{
	QColorDialog::ColorDialogOptions options;
	if (m_withAlpha)
		options |= QColorDialog::ShowAlphaChannel;

	const QColor picked = QColorDialog::getColor(m_color, this, QString(), options);
	if (!picked.isValid() || picked == m_color)
		return;  // cancelled, or the user re-picked the same color

	m_color = picked;
	UpdateSwatch();
	emit colorChanged(m_color);
}

} // namespace neurus
