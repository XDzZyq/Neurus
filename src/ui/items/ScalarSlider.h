/**
 * @file ScalarSlider.h
 * @brief Composite widget pairing a QSlider with a QDoubleSpinBox.
 *
 * Moving either control synchronises the other and emits a single
 * valueChanged() signal, no matter which control was manipulated. The spinbox
 * step and decimals are derived from the range and slider resolution.
 */

#pragma once

#include <QWidget>

class QSlider;
class QDoubleSpinBox;

namespace neurus
{

class ScalarSlider : public QWidget
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs a ScalarSlider with the given range and initial value.
	 *
	 * The spinbox step and decimal places are derived from the range and
	 * slider resolution: step = (max - min) / sliderSteps.
	 *
	 * @param min         Minimum value (spinbox lower bound).
	 * @param max         Maximum value (spinbox upper bound).
	 * @param sliderSteps Number of discrete slider positions (resolution).
	 * @param initial     Initial value for both controls.
	 * @param parent      Parent widget.
	 */
	explicit ScalarSlider(double min, double max, int sliderSteps,
	                      double initial, QWidget* parent = nullptr);
	~ScalarSlider() override = default;

	ScalarSlider(const ScalarSlider&) = delete;
	ScalarSlider& operator=(const ScalarSlider&) = delete;

	/** @brief Returns the current value from the spinbox. */
	double value() const;

	/** @brief Returns the internal QSlider (read-only access for layout tuning). */
	QSlider* slider() const { return m_slider; }

	/** @brief Returns the internal QDoubleSpinBox (read-only access for layout tuning). */
	QDoubleSpinBox* spinBox() const { return m_spin; }

public slots:
	/**
	 * @brief Sets the value on both controls without emitting signals.
	 *
	 * No-op when the value is unchanged.
	 *
	 * @param v New value.
	 */
	void setValue(double v);

signals:
	/** @brief Emitted whenever the slider or spinbox value changes. */
	void valueChanged();

	/** @brief Emitted when the user begins dragging the slider handle (mouse press).
	 *  Bounds one undo entry; fired on the raw mouse-button press so a groove
	 *  click and the drag that follows are one gesture, not two. */
	void pressed();

	/** @brief Emitted when the user releases the slider handle (mouse release).
	 *  Bounds one undo entry. Fired on the slider's raw mouse-button release. */
	void released();

protected:
	/**
	 * @brief Event filter on the inner slider to bracket a drag gesture.
	 *
	 * QSlider's sliderPressed/sliderReleased only fire when the press lands on
	 * the handle; filtering the raw MouseButtonPress/Release guarantees exactly
	 * one pressed()/released() bracket around the whole interaction.
	 */
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	QSlider*        m_slider      = nullptr;
	QDoubleSpinBox* m_spin        = nullptr;
	double          m_min         = 0.0;
	double          m_max         = 1.0;
	int             m_sliderSteps = 100;
	double			m_v			  = 0.0;
};

} // namespace neurus
