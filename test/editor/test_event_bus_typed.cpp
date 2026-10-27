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

} // namespace

/**
 * @brief Tests for the typed EventQueue - no Qt, no GPU required.
 *
 * These tests validate the typed event dispatch system: subscribe, emit,
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
// Core subscribe/emit/Process lifecycle
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, SubscribeAndEmit_HandlerReceivesEvent)
{
	ObjectID* obj = NewObject();
	bool received = false;
	int receivedId = 0;

	m_queue->subscribe<ObjectSelected>(
		[&](const ObjectSelected& e) {
			received = true;
			receivedId = e.objectUid;
		});

	m_queue->enqueue(ObjectSelected{obj->GetObjectID(), 0});

	// Not received yet - deferred dispatch
	EXPECT_FALSE(received);

	m_queue->Process();

	EXPECT_TRUE(received);
	EXPECT_EQ(receivedId, obj->GetObjectID());
}

TEST_F(TypedEventQueueTest, Emit_MultipleHandlersAllReceive)
{
	int callCount = 0;

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { callCount++; });
	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { callCount++; });
	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { callCount++; });

	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
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
		m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
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

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { objectSelectedCount++; });
	m_queue->subscribe<ObjectDeselected>([&](const ObjectDeselected&) { objectDeselectedCount++; });

	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(objectSelectedCount, 2);
	EXPECT_EQ(objectDeselectedCount, 0);
}

TEST_F(TypedEventQueueTest, MultipleEventTypes_EmitAndProcess)
{
	int selectCount = 0;
	int deselectCount = 0;

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { selectCount++; });
	m_queue->subscribe<ObjectDeselected>([&](const ObjectDeselected&) { deselectCount++; });

	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->enqueue(ObjectDeselected{NewObject()->GetObjectID()});

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

	m_queue->subscribe<ObjectSelected>(
		[&](const ObjectSelected& e) { receivedIds.push_back(e.objectUid); });

	m_queue->enqueue(ObjectSelected{o1->GetObjectID(), 0});
	m_queue->enqueue(ObjectSelected{o2->GetObjectID(), 0});
	m_queue->enqueue(ObjectSelected{o3->GetObjectID(), 0});

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

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) {
		outerCount++;
		m_queue->enqueue(ObjectDeselected{obj->GetObjectID()});
	});

	m_queue->subscribe<ObjectDeselected>([&](const ObjectDeselected&) {
		innerCount++;
	});

	m_queue->enqueue(ObjectSelected{obj->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(outerCount, 1);
	EXPECT_EQ(innerCount, 1);
}

TEST_F(TypedEventQueueTest, ReentrantEmit_SameTypeCreatesChain)
{
	int callCount = 0;

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) {
		callCount++;
		if (callCount < 5)
		{
			m_queue->enqueue(ObjectSelected{0, 0});
		}
	});

	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->Process();

	EXPECT_EQ(callCount, 5);
}

// ---------------------------------------------------------------------------
// maxEvents guard
// ---------------------------------------------------------------------------

TEST_F(TypedEventQueueTest, Process_MaxEventsCapPreventsInfiniteLoop)
{
	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) {
		m_queue->enqueue(ObjectSelected{0, 0});
	});

	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});

	EXPECT_NO_THROW({ m_queue->Process(10); });
}

// ---------------------------------------------------------------------------
// Dispatch depth guard
// ---------------------------------------------------------------------------

TEST(EventBusTyped, RunawayEmitNowChainIsStoppedAndRecovers)
{
	EventQueue bus;
	int calls = 0;
	bus.subscribe<LocalRunaway>([&bus, &calls](const LocalRunaway& e) {
		++calls;
		bus.emitNow(e);
	});

	bus.emitNow(LocalRunaway{});

	EXPECT_GT(calls, 0);
	EXPECT_LT(calls, 200);

	// The counter must be restored: a normal dispatch after the runaway chain
	// still reaches its handler.
	int seen = 0;
	bus.subscribe<LocalPing>([&seen](const LocalPing&) { ++seen; });
	bus.emitNow(LocalPing{});
	EXPECT_EQ(seen, 1);
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

	m_queue->subscribe<ObjectSelected>(
		[&](const ObjectSelected& e) { receivedIds.push_back(e.objectUid); });

	m_queue->enqueue(ObjectSelected{o1->GetObjectID(), 0});
	m_queue->enqueue(ObjectSelected{o2->GetObjectID(), 0});
	m_queue->enqueue(ObjectSelected{o3->GetObjectID(), 0});

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

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { selectCount++; });
	m_queue->subscribe<ObjectDeselected>([&](const ObjectDeselected&) { deselectCount++; });

	m_queue->enqueue(ObjectDeselected{NewObject()->GetObjectID()});
	m_queue->Process();

	EXPECT_EQ(selectCount, 0);
	EXPECT_EQ(deselectCount, 1);
}

TEST_F(TypedEventQueueExpandedTest, ObjectDeselected_EmitReceivesCorrectId)
{
	ObjectID* obj = NewObject();
	int receivedId = 0;

	m_queue->subscribe<ObjectDeselected>(
		[&](const ObjectDeselected& e) { receivedId = e.objectUid; });

	m_queue->enqueue(ObjectDeselected{obj->GetObjectID()});
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

	m_queue->subscribe<ObjectSelected>([&](const ObjectSelected&) { selectCount++; });
	m_queue->subscribe<ObjectDeselected>([&](const ObjectDeselected&) { deselectCount++; });

	m_queue->enqueue(ObjectSelected{NewObject()->GetObjectID(), 0});
	m_queue->enqueue(ObjectDeselected{NewObject()->GetObjectID()});

	m_queue->Process();

	EXPECT_EQ(selectCount, 1);
	EXPECT_EQ(deselectCount, 1);
}
