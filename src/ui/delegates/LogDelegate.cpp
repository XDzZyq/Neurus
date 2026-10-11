#include "ui/delegates/LogDelegate.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "core/Log.h"
#include "ui/models/LogModel.h"

namespace neurus
{

LogDelegate::LogDelegate(QObject* parent)
	: QStyledItemDelegate(parent)
{
}

void LogDelegate::setSourcePad(int chars)
{
	m_sourcePad = chars;
}

void LogDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                        const QModelIndex& index) const
{
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, index);

	painter->save();

	// Background: selection > hover > base.
	if (opt.state & QStyle::State_Selected)
		painter->fillRect(opt.rect, opt.palette.highlight());
	else if (opt.state & QStyle::State_MouseOver)
		painter->fillRect(opt.rect, opt.palette.alternateBase());
	else
		painter->fillRect(opt.rect, opt.palette.base());

	const int level = index.data(LogModel::LevelRole).toInt();
	const QColor textColor = (level == static_cast<int>(LogLevel::Error))
	                             ? QColor(211, 47, 47)   // red for errors (#d32f2f)
	                             : QColor(48, 48, 48);   // dark gray for info (#303030)

	QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	if (font.pointSize() < 1)
		font.setPixelSize(12);

	painter->setFont(font);
	painter->setPen(textColor);

	// Compose the row from the three roles so the source field can be
	// right-justified to a fixed column; rightJustified(0) is a no-op.
	const QString ts = index.data(LogModel::TimestampRole).toString();
	const QString src = index.data(LogModel::SourceRole).toString();
	const QString msg = index.data(LogModel::MessageRole).toString();
	const QString line = QStringLiteral("%1  [%2]  %3")
	                         .arg(ts)
	                         .arg(src.rightJustified(m_sourcePad))
	                         .arg(msg);

	const QString elided = QFontMetrics(font).elidedText(
	    line, Qt::ElideRight, opt.rect.width() - 8);
	painter->drawText(opt.rect.adjusted(4, 0, -4, 0),
	                  Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, elided);

	painter->restore();
}

QSize LogDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	Q_UNUSED(option);
	Q_UNUSED(index);
	QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	if (font.pointSize() < 1)
		font.setPixelSize(12);
	return QSize(0, QFontMetrics(font).height() + 4);
}

} // namespace neurus
