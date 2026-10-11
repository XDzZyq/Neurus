/**
 * @file ColorButton.h
 * @brief Color swatch button that opens a QColorDialog.
 *
 * A flat QPushButton painted with the current color; clicking it opens the
 * platform color dialog and, on accept, emits colorChanged() once. The alpha
 * channel is optional (vec4 tints vs vec3 colors).
 */

#pragma once

#include <QColor>
#include <QPushButton>

class QColorDialog;

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
	 * Programmatic pushes must never look like a user edit, so this is silent
	 * by construction.
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

	/** @brief Opens (or raises) the color dialog. */
	void PickColor();

	/** @brief Applies a color chosen in the dialog; emits colorChanged(). */
	void OnColorSelected(const QColor& c);

	QColor m_color{ 255, 255, 255, 255 };
	bool   m_withAlpha = false;

	/// Lazily created and owned by this button. A QColorDialog shown with
	/// open() rather than the static getColor()/exec() so no nested modal
	/// event loop re-enters the app's per-frame render timer.
	QColorDialog* m_dialog = nullptr;
};

} // namespace neurus
