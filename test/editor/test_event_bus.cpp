#include <gtest/gtest.h>

#include <QObject>

#include "editor/events/UIEvents.hpp"

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

TEST_F(UIEventsTest, Singleton_SameInstance)
{
	auto& bus1 = UIEvents::instance();
	auto& bus2 = UIEvents::instance();

	EXPECT_EQ(&bus1, &bus2);
}

TEST_F(UIEventsTest, EmitRenderRequested_CallsConnectedSlot)
{
	bool wasCalled = false;

	QObject::connect(m_ui, &UIEvents::newFrame,
	                 [&wasCalled]() { wasCalled = true; });

	emit m_ui->newFrame();

	EXPECT_TRUE(wasCalled);
}

TEST_F(UIEventsTest, EmitWindowResized_CallsConnectedSlot)
{
	int receivedWidth = 0;
	int receivedHeight = 0;

	QObject::connect(m_ui, &UIEvents::windowResized,
	                 [&receivedWidth, &receivedHeight](int w, int h) {
	                     receivedWidth = w;
	                     receivedHeight = h;
	                 });

	emit m_ui->windowResized(1920, 1080);

	EXPECT_EQ(receivedWidth, 1920);
	EXPECT_EQ(receivedHeight, 1080);
}

TEST_F(UIEventsTest, EmitWindowResized_MultipleSubscribers)
{
	int callCount = 0;

	QObject::connect(m_ui, &UIEvents::windowResized,
	                 [&callCount](int, int) { callCount++; });
	QObject::connect(m_ui, &UIEvents::windowResized,
	                 [&callCount](int, int) { callCount++; });

	emit m_ui->windowResized(800, 600);

	EXPECT_EQ(callCount, 2);
}

TEST_F(UIEventsTest, GpuName_DefaultEmpty)
{
	EXPECT_TRUE(m_ui->gpuName().isEmpty());
}

TEST_F(UIEventsTest, GpuName_UpdateEmitsChanged)
{
	bool wasEmitted = false;
	QObject::connect(m_ui, &UIEvents::gpuNameChanged,
	                 [&wasEmitted]() { wasEmitted = true; });

	m_ui->setGpuName("NVIDIA GeForce RTX 4090");

	EXPECT_EQ(m_ui->gpuName(), QString("NVIDIA GeForce RTX 4090"));
	EXPECT_TRUE(wasEmitted);
}

TEST_F(UIEventsTest, EmitValidationMessage_CallsConnectedSlot)
{
	QString receivedSeverity;
	QString receivedMessage;

	QObject::connect(m_ui, &UIEvents::validationMessage,
	                 [&receivedSeverity, &receivedMessage](QString severity, QString message) {
	                     receivedSeverity = severity;
	                     receivedMessage = message;
	                 });

	emit m_ui->validationMessage("error", "Test error message");

	EXPECT_EQ(receivedSeverity, QString("error"));
	EXPECT_EQ(receivedMessage, QString("Test error message"));
}

TEST_F(UIEventsTest, EmitDeviceLost_CallsConnectedSlot)
{
	bool wasCalled = false;

	QObject::connect(m_ui, &UIEvents::deviceLost,
	                 [&wasCalled]() { wasCalled = true; });

	emit m_ui->deviceLost();

	EXPECT_TRUE(wasCalled);
}

TEST_F(UIEventsTest, MultipleSignals_IndependentChannels)
{
	int renderCount = 0;
	int resizeCount = 0;

	QObject::connect(m_ui, &UIEvents::newFrame,
	                 [&renderCount]() { renderCount++; });
	QObject::connect(m_ui, &UIEvents::windowResized,
	                 [&resizeCount](int, int) { resizeCount++; });

	// Only emit newFrame
	emit m_ui->newFrame();
	emit m_ui->newFrame();

	// Only resizeCount should NOT have changed
	EXPECT_EQ(renderCount, 2);
	EXPECT_EQ(resizeCount, 0);
}
