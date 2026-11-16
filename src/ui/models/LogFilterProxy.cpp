#include "ui/models/LogFilterProxy.hpp"

#include "core/Log.hpp"
#include "ui/models/LogModel.hpp"

namespace neurus
{

LogFilterProxy::LogFilterProxy(QObject* parent)
	: QSortFilterProxyModel(parent)
{
}

void LogFilterProxy::setLevelFilter(LevelFilter filter)
{
	beginFilterChange();
	m_levelFilter = filter;
	endFilterChange();
}

void LogFilterProxy::setSearchText(const QString& text)
{
	beginFilterChange();
	m_searchText = text;
	endFilterChange();
}

bool LogFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
	const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
	if (!idx.isValid())
		return false;

	const int level = idx.data(LogModel::LevelRole).toInt();
	if (m_levelFilter == LevelFilter::InfoOnly && level != static_cast<int>(LogLevel::Info))
		return false;
	if (m_levelFilter == LevelFilter::ErrorsOnly && level != static_cast<int>(LogLevel::Error))
		return false;

	if (!m_searchText.isEmpty())
	{
		const QString msg = idx.data(LogModel::MessageRole).toString();
		if (!msg.contains(m_searchText, Qt::CaseInsensitive))
			return false;
	}
	return true;
}

} // namespace neurus
