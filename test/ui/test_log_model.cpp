#include <gtest/gtest.h>
#include <QAbstractItemModelTester>
#include <QString>

#include <atomic>
#include <thread>

#include "core/Log.h"
#include "ui/models/LogModel.h"
#include "ui/models/LogFilterProxy.h"

namespace neurus
{

class LogModelTest : public ::testing::Test
{
protected:
	void SetUp() override { LogBuffer::instance().Clear(); }
	void TearDown() override { LogBuffer::instance().Clear(); }
};

TEST_F(LogModelTest, RowCountMatchesBuffer)
{
	LogBuffer::instance().Append(LogEntry{LogLevel::Info, {}, "f", 1, "a", 0});
	LogBuffer::instance().Append(LogEntry{LogLevel::Error, {}, "f", 2, "b", 0});

	LogModel model;
	model.Refresh(&LogBuffer::instance());
	EXPECT_EQ(model.rowCount(), 2);
}

TEST_F(LogModelTest, DataRoles)
{
	LogBuffer::instance().Append(
		LogEntry{LogLevel::Info, {}, "myFunc", 42, "hello", 0});
	LogModel model;
	model.Refresh(&LogBuffer::instance());

	const QModelIndex idx = model.index(0, 0);
	EXPECT_EQ(idx.data(LogModel::MessageRole).toString(), QStringLiteral("hello"));
	EXPECT_EQ(idx.data(LogModel::LevelRole).toInt(), static_cast<int>(LogLevel::Info));
	EXPECT_EQ(idx.data(LogModel::SourceRole).toString(), QStringLiteral("myFunc:42"));
	EXPECT_TRUE(idx.data(Qt::DisplayRole).toString().contains(QStringLiteral("hello")));
	EXPECT_GT(idx.data(LogModel::TimestampRole).toString().length(), 0);
	EXPECT_EQ(idx.data(LogModel::SeqRole).toULongLong(), 1u);
}

TEST_F(LogModelTest, RefreshAppendsNewRows)
{
	LogBuffer& buf = LogBuffer::instance();
	buf.Append(LogEntry{LogLevel::Info, {}, "f", 1, "a", 0});
	LogModel model;
	model.Refresh(&buf);
	EXPECT_EQ(model.rowCount(), 1);

	buf.Append(LogEntry{LogLevel::Info, {}, "f", 2, "b", 0});
	model.Refresh(&buf);
	EXPECT_EQ(model.rowCount(), 2);
	EXPECT_EQ(model.index(1, 0).data(LogModel::MessageRole).toString(),
	          QStringLiteral("b"));
}

TEST_F(LogModelTest, ClearResetsModel)
{
	LogBuffer& buf = LogBuffer::instance();
	buf.Append(LogEntry{LogLevel::Info, {}, "f", 1, "a", 0});
	LogModel model;
	model.Refresh(&buf);
	EXPECT_EQ(model.rowCount(), 1);

	buf.Clear();
	model.Refresh(&buf);
	EXPECT_EQ(model.rowCount(), 0);
}

TEST_F(LogModelTest, RingWrapEvictsFromFront)
{
	LogBuffer& buf = LogBuffer::instance();
	for (std::size_t i = 0; i < kLogCapacity; ++i)
		buf.Append(LogEntry{LogLevel::Info, {}, "f", 1, "old", 0});
	LogModel model;
	QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
	model.Refresh(&buf);
	ASSERT_EQ(model.rowCount(), static_cast<int>(kLogCapacity));

	// Several evictions between two refreshes must all leave from the front.
	for (int i = 0; i < 3; ++i)
		buf.Append(LogEntry{LogLevel::Info, {}, "f", 2, "new", 0});
	model.Refresh(&buf);

	EXPECT_EQ(model.rowCount(), static_cast<int>(kLogCapacity));
	EXPECT_EQ(model.index(0, 0).data(LogModel::SeqRole).toULongLong(), 4u);
	const QModelIndex last = model.index(model.rowCount() - 1, 0);
	EXPECT_EQ(last.data(LogModel::SeqRole).toULongLong(), kLogCapacity + 3);
	EXPECT_EQ(last.data(LogModel::MessageRole).toString(), QStringLiteral("new"));
}

// Another thread (the render thread) appends while the UI refreshes. The
// model's count must change only through its own notifications, or the proxy's
// mapping drifts from the source and Qt eventually dereferences a stale index.
TEST_F(LogModelTest, ConcurrentAppendKeepsProxyConsistent)
{
	LogBuffer& buf = LogBuffer::instance();
	LogModel model;
	LogFilterProxy proxy;
	proxy.setSourceModel(&model);
	// No QAbstractItemModelTester here: it also demands that a row's data stay
	// put between notifications, and an entry the writer evicts reads empty
	// until the next Refresh() removes its row. Only the count is structural.

	constexpr int kAppends = static_cast<int>(kLogCapacity) * 2 + 500;
	std::atomic<bool> done{false};
	std::thread writer([&] {
		for (int i = 0; i < kAppends; ++i)
			buf.Append(LogEntry{LogLevel::Info, {}, "w", i, "x", 0});
		done.store(true, std::memory_order_release);
	});

	while (!done.load(std::memory_order_acquire))
	{
		model.Refresh(&buf);
		// EXPECT and break, never ASSERT: returning early would destroy a
		// joinable std::thread and terminate the whole test binary.
		if (proxy.rowCount() != model.rowCount())
		{
			ADD_FAILURE() << "proxy " << proxy.rowCount() << " rows, model " << model.rowCount();
			break;
		}
	}
	writer.join();
	model.Refresh(&buf);

	ASSERT_EQ(model.rowCount(), static_cast<int>(kLogCapacity));
	EXPECT_EQ(proxy.rowCount(), model.rowCount());
	EXPECT_EQ(model.index(0, 0).data(LogModel::SeqRole).toULongLong(),
	          static_cast<qulonglong>(kAppends) - kLogCapacity + 1);
	EXPECT_EQ(model.index(model.rowCount() - 1, 0).data(LogModel::SeqRole).toULongLong(),
	          static_cast<qulonglong>(kAppends));
}

class LogFilterProxyTest : public ::testing::Test
{
protected:
	void SetUp() override { LogBuffer::instance().Clear(); }
	void TearDown() override { LogBuffer::instance().Clear(); }

	LogModel m_model;
	LogFilterProxy m_proxy;

	void PopulateAndRefresh()
	{
		LogBuffer& buf = LogBuffer::instance();
		buf.Append(LogEntry{LogLevel::Info, {}, "f", 1, "geometry loaded", 0});
		buf.Append(LogEntry{LogLevel::Error, {}, "f", 2, "shader failed", 0});
		m_model.Refresh(&buf);
		m_proxy.setSourceModel(&m_model);
	}
};

TEST_F(LogFilterProxyTest, LevelFilter)
{
	PopulateAndRefresh();

	m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::InfoOnly);
	EXPECT_EQ(m_proxy.rowCount(), 1);
	EXPECT_EQ(m_proxy.index(0, 0).data(LogModel::LevelRole).toInt(),
	          static_cast<int>(LogLevel::Info));

	m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::ErrorsOnly);
	EXPECT_EQ(m_proxy.rowCount(), 1);
	EXPECT_EQ(m_proxy.index(0, 0).data(LogModel::LevelRole).toInt(),
	          static_cast<int>(LogLevel::Error));

	m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::All);
	EXPECT_EQ(m_proxy.rowCount(), 2);
}

TEST_F(LogFilterProxyTest, SearchFilterIsCaseInsensitive)
{
	PopulateAndRefresh();

	m_proxy.setSearchText(QStringLiteral("FAILED"));
	EXPECT_EQ(m_proxy.rowCount(), 1);
	EXPECT_EQ(m_proxy.index(0, 0).data(LogModel::MessageRole).toString(),
	          QStringLiteral("shader failed"));

	m_proxy.setSearchText(QStringLiteral("nonexistent"));
	EXPECT_EQ(m_proxy.rowCount(), 0);
}

TEST_F(LogFilterProxyTest, LevelAndSearchCombine)
{
	PopulateAndRefresh();

	m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::ErrorsOnly);
	m_proxy.setSearchText(QStringLiteral("geometry"));
	EXPECT_EQ(m_proxy.rowCount(), 0); // geometry is Info, filter is ErrorsOnly

	m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::All);
	EXPECT_EQ(m_proxy.rowCount(), 1); // geometry only
}

} // namespace neurus
