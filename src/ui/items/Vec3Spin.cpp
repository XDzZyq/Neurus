/**
 * @file Vec3Spin.cpp
 * @brief Vec3Spin implementation — three spinboxes in a horizontal row.
 */

#include "items/Vec3Spin.hpp"

#include <QDoubleSpinBox>
#include <QHBoxLayout>

namespace neurus
{

// =========================================================================
// Constructor
// =========================================================================

Vec3Spin::Vec3Spin(double min, double max, double step, int decimals,
                   const QString& suffix, QWidget* parent)
	: QWidget(parent)
{
	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	auto makeSpin = [&]() {
		auto* spin = new QDoubleSpinBox(this);
		spin->setRange(min, max);
		spin->setSingleStep(step);
		spin->setDecimals(decimals);
		spin->setAlignment(Qt::AlignRight);
		spin->setMinimumWidth(80);
		if (!suffix.isEmpty())
			spin->setSuffix(suffix);
		return spin;
	};

	m_spinX = makeSpin();
	m_spinY = makeSpin();
	m_spinZ = makeSpin();

	layout->addWidget(m_spinX);
	layout->addWidget(m_spinY);
	layout->addWidget(m_spinZ);

	// --- Wire valueChanged signals, each carrying the current triplet ---
	auto emitChanged = [this]() {
		emit valueChanged(
			m_spinX->value(),
			m_spinY->value(),
			m_spinZ->value());
	};
	QObject::connect(m_spinX, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
	                 this, emitChanged);
	QObject::connect(m_spinY, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
	                 this, emitChanged);
	QObject::connect(m_spinZ, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
	                 this, emitChanged);
}

// =========================================================================
// setValue — dirty-checked batch update
// =========================================================================

void Vec3Spin::setValue(double x, double y, double z)
{
	QSignalBlocker bx(m_spinX);
	QSignalBlocker by(m_spinY);
	QSignalBlocker bz(m_spinZ);

	if (m_valX != x)
	{
		m_spinX->setValue(x);
		m_valX = x;
	}
	if (m_valY != y)
	{
		m_spinY->setValue(y);
		m_valY = y;
	}
	if (m_valZ != z)
	{
		m_spinZ->setValue(z);
		m_valZ = z;
	}
}

} // namespace neurus
