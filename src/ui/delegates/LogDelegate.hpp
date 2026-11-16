/**
 * @file LogDelegate.hpp
 * @brief Paints a log row as "HH:MM:SS.mmm  [func:line]  message" in a
 *        fixed-width font, colored by severity (dark gray Info, red Error).
 * @note Composes the row from the TimestampRole / SourceRole / MessageRole
 *       data so the source field can be right-justified to a fixed column
 *       (setSourcePad); keeps selection/hover backgrounds.
 */

#pragma once

#include <QStyledItemDelegate>

namespace neurus
{

/** @brief Delegate for the LogPanel's QListView. */
class LogDelegate : public QStyledItemDelegate
{
	Q_OBJECT

public:
	explicit LogDelegate(QObject* parent = nullptr);

	/**
	 * @brief Sets the monospace column width (in characters) used to
	 *        right-justify the "[func:line]" source field, so message text
	 *        starts at the same X position on every row.
	 * @param chars Padding width; 0 disables padding (default).
	 */
	void setSourcePad(int chars);

	void paint(QPainter* painter, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override;
	QSize sizeHint(const QStyleOptionViewItem& option,
	               const QModelIndex& index) const override;

private:
	int m_sourcePad = 0;
};

} // namespace neurus
