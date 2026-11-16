/**
 * @file LogFilterProxy.hpp
 * @brief QSortFilterProxyModel that filters log rows by severity level and
 *        case-insensitive substring search. Source order is preserved
 *        (sorting is never enabled - logs are chronological).
 */

#pragma once

#include <QSortFilterProxyModel>
#include <QString>

namespace neurus
{

/**
 * @brief Proxy between LogModel and the LogPanel's QListView.
 * @note Only `filterAcceptsRow` is overridden; dynamic sort stays off.
 */
class LogFilterProxy : public QSortFilterProxyModel
{
	Q_OBJECT

public:
	/** @brief Severity visibility filter. */
	enum class LevelFilter
	{
		All,        // show everything
		InfoOnly,   // Info entries only
		ErrorsOnly  // Error entries only
	};

	explicit LogFilterProxy(QObject* parent = nullptr);

	/** @brief Sets the level filter and re-evaluates visible rows. */
	void setLevelFilter(LevelFilter filter);

	/** @brief Sets the case-insensitive substring search (empty = off). */
	void setSearchText(const QString& text);

protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
	LevelFilter m_levelFilter = LevelFilter::All;
	QString     m_searchText;
};

} // namespace neurus
