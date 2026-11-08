#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "editor/events/EventBus.h"
#include "editor/events/SceneEvents.h"
#include "scene/ObjectID.h"

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

	void TearDown() override
	{
		m_queue->Process();
	}

	std::vector<std::unique_ptr<ObjectID>> m_objects;
	EventQueue m_eventBus;
	class EventQueue* m_queue = &m_eventBus;
};

/**
 * @brief Expanded fixture covering concrete editor event payloads.
 */
class TypedEventQueueExpandedTest : public ::testing::Test
{
protected:
	ObjectID* NewObject()
	{
		m_objects.push_back(std::make_unique<ObjectID>());
		return m_objects.back().get();
	}

	void TearDown() override
	{
		m_queue->Process();
	}

	std::vector<std::unique_ptr<ObjectID>> m_objects;
	EventQueue m_eventBus;
	class EventQueue* m_queue = &m_eventBus;
};

// ---------------------------------------------------------------------------
// Core Subscribe/Emit/Process lifecycle
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, SubscribeAndEmit_HandlerReceivesEvent)
{
	ObjectID* obj = NewObject();
	bool received = false;
	int receivedId = 0;

	m_queue->Subscribe<ObjectSelected>(
		[&](const ObjectSelected& e) {
			received = true;
			receivedId = e.objectUid;
		});

	m_queue->Enqueue(ObjectSelected{obj->GetObjectID(), 0});

	// Not received yet - deferred dispatch
	EXPECT_FALSE(received);

	m_queue->Process();

	EXPECT_TRUE(received);
	EXPECT_EQ(receivedId, obj->GetObjectID());
}

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

TEST_F(TypedEventQueueTest, Process_EmptyQueueIsNoOp)
{
	EXPECT_NO_THROW({ m_queue->Process(); });
	EXPECT_NO_THROW({ m_queue->Process(0); });
}

TEST_F(TypedEventQueueTest, Emit_WithoutSubscribersIsNoOp)
{
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
	int objectDeselectedCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { objectSelectedCount++; });
	m_queue->Subscribe<ObjectDeselected>([&](const ObjectDeselected&) { objectDeselectedCount++; });

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(objectSelectedCount, 2);
	EXPECT_EQ(objectDeselectedCount, 0);
}

TEST_F(TypedEventQueueTest, MultipleEventTypes_EmitAndProcess)
{
	int selectCount = 0;
	int deselectCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { selectCount++; });
	m_queue->Subscribe<ObjectDeselected>([&](const ObjectDeselected&) { deselectCount++; });

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Enqueue(ObjectDeselected{NewObject()->GetObjectID()});

	m_queue->Process();

	EXPECT_EQ(selectCount, 1);
	EXPECT_EQ(deselectCount, 1);
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
// Re-entrant emit (handler emits within Process)
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, ReentrantEmit_HandlerEmitsDuringProcess)
{
	ObjectID* obj = NewObject();
	int outerCount = 0;
	int innerCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) {
		outerCount++;
		m_queue->Enqueue(ObjectDeselected{obj->GetObjectID()});
	});

	m_queue->Subscribe<ObjectDeselected>([&](const ObjectDeselected&) {
		innerCount++;
	});

	m_queue->Enqueue(ObjectSelected{obj->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(outerCount, 1);
	EXPECT_EQ(innerCount, 1);
}

TEST_F(TypedEventQueueTest, ReentrantEmit_SameTypeCreatesChain)
{
	int callCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) {
		callCount++;
		if (callCount < 5)
		{
			m_queue->Enqueue(ObjectSelected{0, 0});
		}
	});

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(callCount, 5);
}

// ---------------------------------------------------------------------------
// maxEvents guard
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, Process_MaxEventsCapPreventsInfiniteLoop)
{
	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) {
		m_queue->Enqueue(ObjectSelected{0, 0});
	});

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});

	EXPECT_NO_THROW({ m_queue->Process(10); });
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

TEST(EventBusTyped, ControllerFacingInterfaceCannotDefer)
{
	static_assert(!DeferredEnqueue<IEventQueue>,
	              "IEventQueue must not expose Enqueue: controllers emit synchronously");
	SUCCEED();
}

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

TEST_F(TypedEventQueueTest, EventQueueIsNotCopyable)
{
	static_assert(!std::is_copy_constructible_v<class EventQueue>,
	              "EventQueue must not be copyable");
	static_assert(!std::is_copy_assignable_v<class EventQueue>,
	              "EventQueue must not be copy-assignable");
	SUCCEED();
}

// ===========================================================================
// Expanded tests (UID payloads)
// ===========================================================================

TEST_F(TypedEventQueueExpandedTest, ObjectSelected_MultipleEmits)
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

TEST_F(TypedEventQueueExpandedTest, ObjectDeselected_NoCrossContamination)
{
	int selectCount = 0;
	int deselectCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { selectCount++; });
	m_queue->Subscribe<ObjectDeselected>([&](const ObjectDeselected&) { deselectCount++; });

	m_queue->Enqueue(ObjectDeselected{NewObject()->GetObjectID()});
	m_queue->Process();

	EXPECT_EQ(selectCount, 0);
	EXPECT_EQ(deselectCount, 1);
}

TEST_F(TypedEventQueueExpandedTest, ObjectDeselected_EmitReceivesCorrectId)
{
	ObjectID* obj = NewObject();
	int receivedId = 0;

	m_queue->Subscribe<ObjectDeselected>(
		[&](const ObjectDeselected& e) { receivedId = e.objectUid; });

	m_queue->Enqueue(ObjectDeselected{obj->GetObjectID()});
	m_queue->Process();

	EXPECT_EQ(receivedId, obj->GetObjectID());
}

// ---------------------------------------------------------------------------
// All remaining event types: independent channels
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueExpandedTest, AllNewSignals_IndependentChannels)
{
	int selectCount = 0;
	int deselectCount = 0;

	m_queue->Subscribe<ObjectSelected>([&](const ObjectSelected&) { selectCount++; });
	m_queue->Subscribe<ObjectDeselected>([&](const ObjectDeselected&) { deselectCount++; });

	m_queue->Enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Enqueue(ObjectDeselected{NewObject()->GetObjectID()});

	m_queue->Process();

	EXPECT_EQ(selectCount, 1);
	EXPECT_EQ(deselectCount, 1);
}
