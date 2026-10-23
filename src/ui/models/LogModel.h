/**
 * @file LogModel.h
 * @brief QAbstractListModel wrapping the core LogBuffer for a virtualized
 *        QListView. Polls the buffer in Refresh(); Qt only calls data() for
 *        visible rows.
 */

#pragma once

#include <QAbstractListModel>
#include <chrono>
#include <cstdint>

namespace neurus
{

class LogBuffer;
struct LogEntry;

/**
 * @brief Formats a system_clock time point as "HH:MM:SS.mmm" local time.
 * @param tp The timestamp captured at log emission.
 * @return Formatted string, e.g. "14:03:22.481".
 */
QString FormatLogTimestamp(std::chrono::system_clock::time_point tp);

/**
 * @brief Read-only list model over LogBuffer entries.
 * @note Custom roles let the delegate paint severity color and the proxy
 *       filter by level/message without parsing a combined display string.
 */
class LogModel : public QAbstractListModel
{
	Q_OBJECT

public:
	/** @brief Custom roles exposed per row. */
	enum LogRole
	{
		LevelRole = Qt::UserRole + 1,
		TimestampRole,
		SourceRole,
		MessageRole,
		SeqRole
	};

	explicit LogModel(QObject* parent = nullptr);

	/**
	 * @brief Resyncs the model with the buffer.
	 * @param buffer Current log buffer; null detaches (empties the model).
	 * @note Rows map to seqs: row r is entry m_firstSeq + r. Entries the ring
	 *       evicted leave from the front, new entries append at the end
	 *       (scroll position preserved); a seq drop (Clear) resets the model.
	 *       The buffer may grow on any thread while this runs; the model
	 *       only moves its own count, between the matching notifications.
	 */
	void Refresh(const LogBuffer* buffer);

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

	/**
	 * @brief Width in characters of the longest "func:line" source string
	 *        currently in the buffer (0 when empty).
	 * @note Updated on every Refresh(); the delegate pads the source column
	 *       to this width so message text starts at the same X on every row.
	 */
	int MaxSourceChars() const { return m_maxSourceChars; }

private:
	const LogBuffer* m_buffer = nullptr;
	uint64_t         m_lastSeq = 0;
	uint64_t         m_firstSeq = 0;      // seq of row 0
	int              m_rows = 0;          // count announced to views
	int              m_maxSourceChars = 0;

	/** @brief Widens m_maxSourceChars over rows [begin, end). */
	void MeasureRows(int begin, int end);

	/**
	 * @brief Character width of an entry's "func:line" source field.
	 * @param e The log entry to measure.
	 * @return Length of "%func:line" in characters.
	 */
	int SourceLength(const LogEntry& e) const;
};

} // namespace neurus
