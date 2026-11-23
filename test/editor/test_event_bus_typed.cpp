#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "editor/events/EventBus.hpp"
#include "editor/events/SceneEvents.hpp"
#include "scene/ObjectID.hpp"

using namespace neurus;

namespace {

/** @brief Event whose handler re-emits itself, to exercise the depth guard. */
struct LocalRunaway
{
};

/** @brief Plain local event, used to check the depth counter is restored. */
struct LocalPing
{
};

/** @brief Reply event, used to observe synchronous vs. deferred dispatch order. */
struct LocalPong
{
};

/**
 * @brief Detects a deferred emit surface on a queue type.
 *
 * A bare requires-expression is only false for an invalid expression in a
 * dependent context: written directly in a test body the missing member would be
 * a hard error, so the check has to live in a concept.
 */
template<typename Queue>
concept DeferredEnqueue = requires(Queue& q, const LocalPing& e) { q.Enqueue(e); };

} // namespace

/**
 * @brief Tests for the typed EventQueue - no Qt, no GPU required.
 *
 * These tests validate the typed event dispatch system: Subscribe, Emit,
 * deferred Process(), independent event channels, and handler ordering.
 *
 * ObjectSelected / ObjectDeselected carry integer object UIDs; NewObject()
 * supplies fresh ObjectIDs (held by the fixture so their UIDs stay stable).
 */
class TypedEventQueueTest : public ::testing::Test
{
protected:
	/** @brief Creates a fresh ObjectID held by the fixture (ObjectID is non-copyable). */
	ObjectID* NewObject()
	{
		m_objects.push_back(std::make_unique<ObjectID>());
		return m_objects.back().get();
	}


	std::vector<std::unique_ptr<ObjectID>> m_objects;
	EventQueue m_eventBus;
	class EventQueue* m_queue = &m_eventBus;
};

// ---------------------------------------------------------------------------
// Core Subscribe/Emit/Process lifecycle
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, Emit_MultipleHandlersAllReceive)
{
	int callCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { callCount++; });
	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { callCount++; });
	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { callCount++; });

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(callCount, 3);
}

TEST_F(TypedEventQueueTest, ProcessingWithoutConsumersIsHarmless)
{
	EXPECT_NO_THROW({ m_queue->Process(); });
	EXPECT_NO_THROW({ m_queue->Process(0); });

	EXPECT_NO_THROW({
		m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
		m_queue->Process();
	});
}

// ---------------------------------------------------------------------------
// Independent event channels
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, DifferentEventTypes_IndependentChannels)
{
	int objectSelectedCount = 0;
	int deselectedUid = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { objectSelectedCount++; });
	m_queue->Subscribe<ObjectDeselected>([&](const ObjectDeselected& e) { deselectedUid = e.objectUid; });

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(objectSelectedCount, 2);
	EXPECT_EQ(deselectedUid, 0);
	const int uid = NewObject()->GetObjectID();
	m_queue->Enqueue(ObjectDeselected{uid});
	m_queue->Process();
	EXPECT_EQ(deselectedUid, uid);
	EXPECT_EQ(objectSelectedCount, 2);
}

// ---------------------------------------------------------------------------
// Multiple emits, single Process
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, MultipleEmits_ProcessedInOrder)
{
	ObjectID* o1 = NewObject();
	ObjectID* o2 = NewObject();
	ObjectID* o3 = NewObject();
	std::vector<int> receivedIds;

	m_queue->Subscribe<ObjectSelected>(
		[&](const ObjectSelected& e) { receivedIds.push_back(e.objectUid); });

	m_queue->Enqueue(ObjectSelected{o1->GetObjectID(), 0});
	m_queue->Enqueue(ObjectSelected{o2->GetObjectID(), 0});
	m_queue->Enqueue(ObjectSelected{o3->GetObjectID(), 0});

	m_queue->Process();

	ASSERT_EQ(receivedIds.size(), 3);
	EXPECT_EQ(receivedIds[0], o1->GetObjectID());
	EXPECT_EQ(receivedIds[1], o2->GetObjectID());
	EXPECT_EQ(receivedIds[2], o3->GetObjectID());
}

// ---------------------------------------------------------------------------
// maxEvents guard
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, ProcessingAReentrantQueueHonorsTheDrainLimit)
{
	int handled = 0;
	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) {
		++handled;
		if (handled < 20)
			m_queue->Enqueue(ObjectSelected{0, 0});
	});
	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process(10);
	EXPECT_EQ(handled, 10);
	m_queue->Process();
	EXPECT_EQ(handled, 20);
	EXPECT_EQ(m_queue->PendingCount(), 0u);
}

// ---------------------------------------------------------------------------
// Dispatch depth guard
// ---------------------------------------------------------------------------

TEST(EventBusTyped, RunawayEmitChainIsStoppedAndRecovers)
{
	EventQueue bus;
	int calls = 0;
	bus.Subscribe<LocalRunaway>([&bus, &calls](const LocalRunaway& e) {
		++calls;
		bus.Emit(e);
	});

	bus.Emit(LocalRunaway{});

	EXPECT_GT(calls, 0);
	EXPECT_LT(calls, 200);

	// The counter must be restored: a normal dispatch after the runaway chain
	// still reaches its handler.
	int seen = 0;
	bus.Subscribe<LocalPing>([&seen](const LocalPing&) { ++seen; });
	bus.Emit(LocalPing{});
	EXPECT_EQ(seen, 1);
}

// ---------------------------------------------------------------------------
// Synchronous dispatch contract
// ---------------------------------------------------------------------------

static_assert(!DeferredEnqueue<IEventQueue>,
              "IEventQueue must not expose Enqueue: controllers emit synchronously");

TEST(EventBusTyped, HandlerEmittedEventsCompleteBeforeTheHandlerReturns)
{
	EventQueue bus;
	std::vector<int> order;

	bus.Subscribe<LocalPing>([&](const LocalPing&) {
		order.push_back(1);
		bus.Emit(LocalPong{});
		order.push_back(2);
	});
	bus.Subscribe<LocalPong>([&](const LocalPong&) { order.push_back(3); });

	bus.Emit(LocalPing{});
	EXPECT_EQ(order, (std::vector<int>{ 1, 3, 2 }));
}

TEST(EventBusTyped, UiBoundaryEnqueueStaysDeferred)
{
	EventQueue bus;
	std::vector<int> order;

	bus.Subscribe<LocalPing>([&](const LocalPing&) {
		order.push_back(1);
		bus.Enqueue(LocalPong{});
		order.push_back(2);
	});
	bus.Subscribe<LocalPong>([&](const LocalPong&) { order.push_back(3); });

	bus.Enqueue(LocalPing{});
	EXPECT_TRUE(order.empty());
	bus.Process();
	EXPECT_EQ(order, (std::vector<int>{ 1, 2, 3 }));
}

// ---------------------------------------------------------------------------
// EventQueue is NOT copyable (compile-time check)
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<class EventQueue>,
              "EventQueue must not be copyable");
static_assert(!std::is_copy_assignable_v<class EventQueue>,
              "EventQueue must not be copy-assignable");
