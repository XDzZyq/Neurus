#include <gtest/gtest.h>

#include <QObject>

#include "ui/events/UIEvents.hpp"

using namespace neurus;

/**
 * @brief Tests for the UIEvents singleton (Qt signal bus for UI↔Editor).
 *
 * UIEvents carries rendering lifecycle signals that are Qt-dependent by
 * design - QTimer, QWindow, and the Qt event loop drive these signals.
 *
 * For typed Editor↔Renderer events, see test_event_bus_typed.cpp.
 */
class UIEventsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_ui = &UIEvents::instance();
	}

	UIEvents* m_ui = nullptr;
};

TEST_F(UIEventsTest, GpuName_UpdateEmitsChanged)
{
	const QString previous = m_ui->gpuName();
	QObject receiver;
	bool wasEmitted = false;
	QObject::connect(m_ui, &UIEvents::gpuNameChanged, &receiver,
	                 [&wasEmitted]() { wasEmitted = true; });

	m_ui->setGpuName("NVIDIA GeForce RTX 4090");

	EXPECT_EQ(m_ui->gpuName(), QString("NVIDIA GeForce RTX 4090"));
	EXPECT_TRUE(wasEmitted);
	m_ui->setGpuName(previous);
}
