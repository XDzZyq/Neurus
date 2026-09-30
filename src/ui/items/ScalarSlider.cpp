/**
 * @file ScalarSlider.cpp
 * @brief ScalarSlider implementation — bidirectional slider↔spin sync.
 */

#include "items/ScalarSlider.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QSlider>

#include <cmath>

namespace neurus
{

// =========================================================================
// Constructor
// =========================================================================

ScalarSlider::ScalarSlider(double min, double max, int sliderSteps,
                           double initial, QWidget* parent)
	: QWidget(parent)
	, m_min(min)
	, m_max(max)
	, m_sliderSteps(sliderSteps)
	, m_v(initial)
{
	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	// --- Slider ---
	m_slider = new QSlider(Qt::Horizontal, this);
	m_slider->setRange(0, sliderSteps);
	m_slider->setValue(static_cast<int>((initial - min) / (max - min) * sliderSteps));
	m_slider->setTickPosition(QSlider::TicksBelow);
	m_slider->setTickInterval(std::max(1, sliderSteps / 10));

	// --- Spinbox ---
	const double step = (max - min) / sliderSteps;
	const int decimals = static_cast<int>(std::ceil(-std::log10(step)));

	m_spin = new QDoubleSpinBox(this);
	m_spin->setRange(min, max);
	m_spin->setSingleStep(step);
	m_spin->setDecimals(decimals);
	m_spin->setValue(initial);
	m_spin->setMinimumWidth(70);

	layout->addWidget(m_slider);
	layout->addWidget(m_spin);

	// --- Bidirectional sync: slider → spinbox ---
	connect(m_slider, &QSlider::valueChanged, this, [this](int val) {
		double d = m_min + (m_max - m_min) * val / m_sliderSteps;
		m_spin->blockSignals(true);
		m_spin->setValue(d);
		m_spin->blockSignals(false);
		// m_v mirrors what the controls show, user edits included: it guards
		// setValue(), and a shadow copy that only tracked programmatic writes
		// would make a later push of that same value a silent no-op while the
		// widget still displayed the user's number.
		m_v = m_spin->value();
		emit valueChanged();
	});

	// --- Bidirectional sync: spinbox → slider ---
	connect(m_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
	        this, [this](double val) {
		int s = static_cast<int>((val - m_min) / (m_max - m_min) * m_sliderSteps);
		m_slider->blockSignals(true);
		m_slider->setValue(s);
		m_slider->blockSignals(false);
		m_v = val;
		emit valueChanged();
	});

	// --- Gesture boundaries: bracket the whole mouse interaction ---
	// Filter raw press/release on the slider rather than forwarding
	// sliderPressed/sliderReleased, which skip groove clicks and would split a
	// click+drag into two undo entries.
	m_slider->installEventFilter(this);
}

// =========================================================================
// Accessors
// =========================================================================

double ScalarSlider::value() const
{
	return m_spin->value();
}

void ScalarSlider::setValue(double v)
{
	if (m_v == v)
		return;

	QSignalBlocker b1(m_slider);
	QSignalBlocker b2(m_spin);
	m_spin->setValue(v);
	m_slider->setValue(static_cast<int>((v - m_min) / (m_max - m_min) * m_sliderSteps));
	
	m_v = v;
}

// =========================================================================
// Gesture bracketing
// =========================================================================

bool ScalarSlider::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == m_slider)
	{
		if (event->type() == QEvent::MouseButtonPress)
		{
			if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
				emit pressed();
		}
		else if (event->type() == QEvent::MouseButtonRelease)
		{
			if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
				emit released();
		}
	}
	return QWidget::eventFilter(watched, event);
}

} // namespace neurus
