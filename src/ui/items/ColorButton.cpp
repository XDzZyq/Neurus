/**
 * @file ColorButton.cpp
 * @brief ColorButton implementation.
 */

#include "items/ColorButton.h"

#include <QColorDialog>
#include <QDialog>

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
	// checkerboard: the swatch is a hint at the hue, not a preview.
	const QColor opaque(m_color.red(), m_color.green(), m_color.blue());
	const QString border = opaque.lightnessF() > 0.5 ? "#404040" : "#a0a0a0";
	setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid %2; "
	                      "border-radius: 2px; }")
	                  .arg(opaque.name(), border));
	setToolTip(m_withAlpha ? m_color.name(QColor::HexArgb) : opaque.name());
}

void ColorButton::PickColor()
{
	// One reusable dialog, shown with open() - window-modal and asynchronous -
	// instead of the static QColorDialog::getColor(), whose exec() spins a
	// nested modal event loop that re-enters the app's per-frame render timer.
	if (!m_dialog)
	{
		m_dialog = new QColorDialog(m_color, this);
		m_dialog->setOption(QColorDialog::ShowAlphaChannel, m_withAlpha);
		QObject::connect(m_dialog, &QColorDialog::colorSelected,
		                 this, &ColorButton::OnColorSelected);
	}

	m_dialog->setCurrentColor(m_color);
	// QColorDialog declares its own open(receiver, member) overloads, which
	// hide QDialog::open(); name the base explicitly.
	static_cast<QDialog*>(m_dialog)->open();
	m_dialog->raise();
	m_dialog->activateWindow();
}

void ColorButton::OnColorSelected(const QColor& c)
{
	if (!c.isValid() || c == m_color)
		return;  // cancelled, or the user re-picked the same color

	m_color = c;
	UpdateSwatch();
	emit colorChanged(m_color);
}

} // namespace neurus
