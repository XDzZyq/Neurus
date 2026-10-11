#include "ui/models/LogModel.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <QString>

#include "core/Log.h"

namespace neurus
{

QString FormatLogTimestamp(std::chrono::system_clock::time_point tp)
{
	const std::time_t t = std::chrono::system_clock::to_time_t(tp);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                    tp.time_since_epoch()).count() % 1000;
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
	              tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms));
	return QString::fromLatin1(buf);
}

LogModel::LogModel(QObject* parent)
	: QAbstractListModel(parent)
{
}

int LogModel::SourceLength(const LogEntry& e) const
{
	return QStringLiteral("%1:%2").arg(e.func).arg(e.line).length();
}

void LogModel::Refresh(const LogBuffer* buffer)
{
	if (!buffer)
	{
		if (m_buffer)
		{
			beginResetModel();
			m_buffer = nullptr;
			m_lastSeq = 0;
			m_maxSourceChars = 0;
			endResetModel();
		}
		return;
	}

	const uint64_t newSeq = buffer->HeadSeq();
	const int newRows = static_cast<int>(buffer->Size());

	if (!m_buffer)
	{
		// First attach: populate everything.
		if (newRows > 0)
		{
			beginInsertRows(QModelIndex(), 0, newRows - 1);
			m_buffer = buffer;
			m_lastSeq = newSeq;
			m_maxSourceChars = 0;
			for (int i = 0; i < newRows; ++i)
				m_maxSourceChars = std::max(m_maxSourceChars,
				                            SourceLength(buffer->At(static_cast<std::size_t>(i))));
			endInsertRows();
		}
		else
		{
			// Empty buffer: attach without emitting an invalid (0, -1)
			// insert range.
			m_buffer = buffer;
			m_lastSeq = newSeq;
			m_maxSourceChars = 0;
		}
		return;
	}

	if (newSeq < m_lastSeq)
	{
		// Buffer cleared (seq counter reset): full reset.
		beginResetModel();
		m_buffer = buffer;
		m_lastSeq = newSeq;
		m_maxSourceChars = 0;
		endResetModel();
		return;
	}

	const int oldRows = rowCount();
	if (newRows > oldRows)
	{
		// Pure append: insert the delta at the end.
		beginInsertRows(QModelIndex(), oldRows, newRows - 1);
		m_buffer = buffer;
		m_lastSeq = newSeq;
		for (int i = oldRows; i < newRows; ++i)
			m_maxSourceChars = std::max(m_maxSourceChars,
			                            SourceLength(buffer->At(static_cast<std::size_t>(i))));
		endInsertRows();
		return;
	}

	// Size unchanged but seq advanced: the ring wrapped (oldest replaced by
	// newest). Drop the oldest row and append the newest instead of resetting
	// the model, so a view scrolled into history keeps its position.
	if (newSeq > m_lastSeq)
	{
		beginRemoveRows(QModelIndex(), 0, 0);
		endRemoveRows();
		beginInsertRows(QModelIndex(), newRows - 1, newRows - 1);
		m_buffer = buffer;
		m_lastSeq = newSeq;
		m_maxSourceChars = std::max(m_maxSourceChars,
		                            SourceLength(buffer->At(static_cast<std::size_t>(newRows - 1))));
		endInsertRows();
	}
	else
	{
		m_buffer = buffer;
		m_lastSeq = newSeq;
	}
}

int LogModel::rowCount(const QModelIndex& parent) const
{
	return (parent.isValid() || !m_buffer) ? 0 : static_cast<int>(m_buffer->Size());
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || !m_buffer)
		return QVariant();

	const int row = index.row();
	if (row < 0 || row >= static_cast<int>(m_buffer->Size()))
		return QVariant();

	const LogEntry entry = m_buffer->At(static_cast<std::size_t>(row));

	switch (role)
	{
	case Qt::DisplayRole:
		return QStringLiteral("%1  [%2]  %3")
			.arg(FormatLogTimestamp(entry.timestamp))
			.arg(QStringLiteral("%1:%2").arg(entry.func).arg(entry.line))
			.arg(QString::fromStdString(entry.message));
	case LevelRole:
		return static_cast<int>(entry.level);
	case TimestampRole:
		return FormatLogTimestamp(entry.timestamp);
	case SourceRole:
		return QStringLiteral("%1:%2").arg(entry.func).arg(entry.line);
	case MessageRole:
		return QString::fromStdString(entry.message);
	case SeqRole:
		return static_cast<qulonglong>(entry.seq);
	default:
		return QVariant();
	}
}

} // namespace neurus
