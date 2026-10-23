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
			m_firstSeq = 0;
			m_rows = 0;
			m_maxSourceChars = 0;
			endResetModel();
		}
		return;
	}

	// Read seq and size together: the render thread appends concurrently, and
	// a seq read apart from the size could describe a different buffer.
	const LogBuffer::State state = buffer->Snapshot();
	const uint64_t newSeq = state.headSeq;
	const uint64_t newFirst = state.size > 0 ? newSeq - state.size + 1 : newSeq + 1;

	if (!m_buffer || newSeq < m_lastSeq)
	{
		// First attach, or a Clear (seq counter reset): rebuild from scratch.
		beginResetModel();
		m_buffer = buffer;
		m_lastSeq = newSeq;
		m_firstSeq = newFirst;
		m_rows = static_cast<int>(state.size);
		m_maxSourceChars = 0;
		MeasureRows(0, m_rows);
		endResetModel();
		return;
	}

	// Rows whose entries the ring evicted since the last refresh leave from
	// the front, so a view scrolled into history keeps its position.
	if (m_rows > 0 && newFirst > m_firstSeq)
	{
		const uint64_t gone = newFirst - m_firstSeq;
		const int removed = gone >= static_cast<uint64_t>(m_rows) ? m_rows : static_cast<int>(gone);
		beginRemoveRows(QModelIndex(), 0, removed - 1);
		m_firstSeq += static_cast<uint64_t>(removed);
		m_rows -= removed;
		endRemoveRows();
	}
	if (m_rows == 0)
	{
		m_firstSeq = newFirst;
	}

	// New entries append at the end.
	const int target = static_cast<int>(newSeq + 1 - m_firstSeq);
	if (target > m_rows)
	{
		const int oldRows = m_rows;
		beginInsertRows(QModelIndex(), oldRows, target - 1);
		m_rows = target;
		MeasureRows(oldRows, target);
		endInsertRows();
	}
	m_lastSeq = newSeq;
}

void LogModel::MeasureRows(int begin, int end)
{
	LogEntry entry{};
	for (int row = begin; row < end; ++row)
	{
		if (m_buffer->BySeq(m_firstSeq + static_cast<uint64_t>(row), entry))
			m_maxSourceChars = std::max(m_maxSourceChars, SourceLength(entry));
	}
}

int LogModel::rowCount(const QModelIndex& parent) const
{
	// The model's own count, never the live buffer size: other threads append
	// at any time, and Qt requires the count to change only between the
	// begin/end notifications Refresh() emits.
	return (parent.isValid() || !m_buffer) ? 0 : m_rows;
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || !m_buffer)
		return QVariant();

	const int row = index.row();
	if (row < 0 || row >= m_rows)
		return QVariant();

	// By seq, not by ring position: a concurrent append that wraps the ring
	// shifts every position, but a seq names the same entry until evicted.
	LogEntry entry{};
	if (!m_buffer->BySeq(m_firstSeq + static_cast<uint64_t>(row), entry))
		return QVariant();

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
