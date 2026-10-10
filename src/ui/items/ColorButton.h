/**
 * @file ColorButton.h
 * @brief Color swatch button that opens a QColorDialog.
 *
 * A flat QPushButton painted with the current color; clicking it opens the
 * platform color dialog and, on accept, emits colorChanged() once. The only
 * color-editing primitive in the UI layer — before it, every colored property
 * (light color, debug tint) had no way to be edited from a panel.
 *
 * Architecture:
 * - QPushButton subclass; the swatch is a stylesheet background, not a painted
 *   pixmap, so it follows the widget's own enabled/disabled rendering.
 * - Optional alpha channel: debug objects tint with a vec4, lights with a vec3.
 * - No Vulkan, Renderer or Scene dependencies — pure Qt UI layer.
 */

#pragma once

#include <QColor>
#include <QPushButton>

namespace neurus
{

class ColorButton : public QPushButton
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs the swatch.
	 * @param withAlpha true to show the dialog's alpha channel slider.
	 * @param parent    Parent widget.
	 */
	explicit ColorButton(bool withAlpha = false, QWidget* parent = nullptr);
	~ColorButton() override = default;

	ColorButton(const ColorButton&) = delete;
	ColorButton& operator=(const ColorButton&) = delete;

	/** @brief Returns the currently shown color. */
	const QColor& color() const { return m_color; }

public slots:
	/**
	 * @brief Sets the shown color without emitting colorChanged().
	 *
	 * Programmatic updates (a Refresh() push from the panel) must never look
	 * like a user edit, so this is silent by construction rather than relying on
	 * the caller to block signals.
	 *
	 * @param c New color.
	 */
	void setColor(const QColor& c);

signals:
	/** @brief Emitted once per accepted dialog, with the chosen color. */
	void colorChanged(const QColor& c);

private:
	/** @brief Repaints the swatch from m_color. */
	void UpdateSwatch();

	/** @brief Opens the dialog; emits colorChanged() only on accept + change. */
	void PickColor();

	QColor m_color{ 255, 255, 255, 255 };
	bool   m_withAlpha = false;
};

} // namespace neurus
