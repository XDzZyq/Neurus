/**
 * @file test_render_request_queue.cpp
 * @brief The handoff contract of RenderRequestQueue: strict FIFO within a
 *        batch, whole-batch swap-out, next-batch isolation, and move-through.
 *
 * The queue is the single point of handoff between the Editor (posting GPU work
 * as values) and the renderer (draining a batch per frame). Three properties
 * carry that contract, and each is easy to "pass" by accident unless the
 * assertion names the thing it depends on:
 *
 *   1. **Strict FIFO within a batch.** RenderRequests.h relies on append order
 *      being execution order (an upload posted after DropSceneResourcesRequest
 *      lands in the new scene). Every request below carries a distinguishing
 *      objectId/uid, and the batch is checked index-by-index - a queue that
 *      reordered, or a stack that reversed, would fail rather than pass on
 *      "all the requests came back".
 *   2. **Whole-batch swap-out.** TakeAll() leaves with the storage, so a Post()
 *      arriving while the caller executes a taken batch lands in the NEXT
 *      TakeAll(), never mid-iteration. Asserted structurally: a request posted
 *      between two TakeAll() calls appears only in the second batch.
 *   3. **Move-through, never copy.** A request owns vectors (SPIR-V) and maps
 *      (lighting). Post() takes an rvalue and TakeAll() swaps; neither should
 *      duplicate the payload. Pinned by capturing the SPIR-V vector's data()
 *      before posting and asserting the drained request reports the same buffer
 *      address - true only if the storage was stolen end to end.
 *
 * Headless: the queue touches no GPU, so these run without a Vulkan device.
 */

#include <gtest/gtest.h>

#include "core/BatchQueue.h"
#include "render/RenderRequestQueue.h"
#include "render/RenderRequests.h"
#include "render/resources/LightingCache.h"
#include "scene/Light.h"

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

using namespace neurus;

namespace
{
	// Pulls the objectId out of whichever request alternative carries one, so a
	// drained batch can be checked by a single identifying number per entry.
	// A request kind without an objectId returns a sentinel the tests never use.
	int RequestObjectId(const RenderRequest& r)
	{
		if (const auto* m = std::get_if<UploadMeshRequest>(&r)) return m->objectId;
		if (const auto* e = std::get_if<UploadEnvironmentRequest>(&r)) return e->objectId;
		if (const auto* l = std::get_if<CreateLightGpuRequest>(&r)) return l->uid;
		if (const auto* u = std::get_if<UpdateLightRequest>(&r)) return u->uid;
		return -9999;
	}
}

// ---------------------------------------------------------------------------
// 1. Empty on construction, and Empty() tracks Post / TakeAll.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, EmptyTracksPostAndTake)
{
	RenderRequestQueue queue;
	EXPECT_TRUE(queue.Empty());

	queue.Push(UploadMeshRequest{.objectId = 1});
	EXPECT_FALSE(queue.Empty());

	// A second Push keeps it non-empty: Empty() reflects the whole batch, not
	// the last Push.
	queue.Push(UploadMeshRequest{.objectId = 2});
	EXPECT_FALSE(queue.Empty());

	auto batch = queue.TakeAll();
	EXPECT_EQ(batch.size(), 2u);
	// TakeAll drained everything: the queue is empty again.
	EXPECT_TRUE(queue.Empty());
}

// ---------------------------------------------------------------------------
// 2. Strict FIFO: a batch comes back in append order, oldest first.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, TakeAllPreservesFifoOrder)
{
	RenderRequestQueue queue;

	// A deliberately interleaved sequence across request kinds: order is the
	// contract, independent of alternative. DropScene between two uploads is the
	// exact ordering RenderRequests.h calls out.
	queue.Push(UploadMeshRequest{.objectId = 10});
	queue.Push(CreateLightGpuRequest{.uid = 20, .type = LightType::SUNLIGHT});
	queue.Push(DropSceneResourcesRequest{});
	queue.Push(UploadMeshRequest{.objectId = 30});
	queue.Push(UpdateLightRequest{.uid = 40, .light = SunLightStruct{}});

	auto batch = queue.TakeAll();
	ASSERT_EQ(batch.size(), 5u);

	// Index-by-index: a stack (reversed) or a reordering queue fails here.
	EXPECT_EQ(RequestObjectId(batch[0]), 10);
	EXPECT_TRUE(std::holds_alternative<UploadMeshRequest>(batch[0]));

	EXPECT_EQ(RequestObjectId(batch[1]), 20);
	EXPECT_TRUE(std::holds_alternative<CreateLightGpuRequest>(batch[1]));

	EXPECT_TRUE(std::holds_alternative<DropSceneResourcesRequest>(batch[2]));

	EXPECT_EQ(RequestObjectId(batch[3]), 30);
	EXPECT_TRUE(std::holds_alternative<UploadMeshRequest>(batch[3]));

	EXPECT_EQ(RequestObjectId(batch[4]), 40);
	EXPECT_TRUE(std::holds_alternative<UpdateLightRequest>(batch[4]));
}

// ---------------------------------------------------------------------------
// 3. Whole-batch isolation: a Post between two TakeAll() lands in the second
//    batch only. This is the guarantee that lets a handler post a follow-up
//    without the executor tripping over the container it is walking.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, PostDuringProcessingLandsInNextBatch)
{
	RenderRequestQueue queue;

	queue.Push(UploadMeshRequest{.objectId = 100});
	queue.Push(UploadMeshRequest{.objectId = 101});

	// First drain takes the storage; the returned vector is now independent of
	// the queue's internal (freshly empty) vector.
	auto first = queue.TakeAll();
	ASSERT_EQ(first.size(), 2u);
	EXPECT_EQ(RequestObjectId(first[0]), 100);
	EXPECT_EQ(RequestObjectId(first[1]), 101);

	// A Push now - standing in for a handler enqueuing a follow-up while the
	// first batch is still being executed.
	queue.Push(UploadMeshRequest{.objectId = 200});

	// The new request did not appear in the already-taken batch ...
	EXPECT_EQ(first.size(), 2u);
	// ... it is waiting in the next one.
	auto second = queue.TakeAll();
	ASSERT_EQ(second.size(), 1u);
	EXPECT_EQ(RequestObjectId(second[0]), 200);
}

// ---------------------------------------------------------------------------
// 4. Repeated TakeAll on a drained queue returns empty, never the prior batch.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, TakeAllAfterDrainReturnsEmpty)
{
	RenderRequestQueue queue;
	queue.Push(UploadMeshRequest{.objectId = 7});

	auto first = queue.TakeAll();
	EXPECT_EQ(first.size(), 1u);

	// The storage left with the first batch; a second TakeAll sees a fresh,
	// empty vector - it must not hand back the already-drained request.
	auto second = queue.TakeAll();
	EXPECT_TRUE(second.empty());
}

// ---------------------------------------------------------------------------
// 5. Variant payload survives the round trip: an UpdateLightingRequest's map is
//    intact, keys and struct contents preserved.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, LightingMapPayloadSurvives)
{
	RenderRequestQueue queue;

	UpdateLightingRequest req;
	SunLightStruct sun{};
	sun.power = 3.5f;
	PointLightStruct point{};
	point.radius = 2.25f;
	req.lights.emplace(5, sun);
	req.lights.emplace(6, point);

	queue.Push(std::move(req));

	auto batch = queue.TakeAll();
	ASSERT_EQ(batch.size(), 1u);
	const auto* drained = std::get_if<UpdateLightingRequest>(&batch[0]);
	ASSERT_NE(drained, nullptr);
	ASSERT_EQ(drained->lights.size(), 2u);

	// Keys and the carried struct fields come through unchanged.
	ASSERT_TRUE(drained->lights.count(5));
	const auto& s = std::get<SunLightStruct>(drained->lights.at(5));
	EXPECT_FLOAT_EQ(s.power, 3.5f);

	ASSERT_TRUE(drained->lights.count(6));
	const auto& p = std::get<PointLightStruct>(drained->lights.at(6));
	EXPECT_FLOAT_EQ(p.radius, 2.25f);
}

// ---------------------------------------------------------------------------
// 6. Move-through: the SPIR-V vector is moved, never copied, by Post and
//    TakeAll. Captured data() before posting must equal the drained request's
//    data() - only a move preserves the buffer address end to end.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, SpirvPayloadMovedNotCopied)
{
	RenderRequestQueue queue;

	EnsurePipelineRequest req;
	req.shaderId = 11;
	req.version  = 3;
	req.vertexSpv   = std::vector<uint32_t>(64, 0xABu);
	req.fragmentSpv = std::vector<uint32_t>(32, 0xCDu);

	// Capture the storage addresses before the request leaves our hands.
	const uint32_t* vertexData   = req.vertexSpv.data();
	const uint32_t* fragmentData = req.fragmentSpv.data();
	ASSERT_NE(vertexData, nullptr);
	ASSERT_NE(fragmentData, nullptr);

	queue.Push(std::move(req));

	auto batch = queue.TakeAll();
	ASSERT_EQ(batch.size(), 1u);
	const auto* drained = std::get_if<EnsurePipelineRequest>(&batch[0]);
	ASSERT_NE(drained, nullptr);

	EXPECT_EQ(drained->shaderId, 11);
	EXPECT_EQ(drained->version, 3);
	ASSERT_EQ(drained->vertexSpv.size(), 64u);
	ASSERT_EQ(drained->fragmentSpv.size(), 32u);

	// The decisive check: same buffer, so Post + TakeAll moved the vector's
	// storage rather than reallocating a copy. A copy anywhere on the path would
	// change the address and fail here.
	EXPECT_EQ(drained->vertexSpv.data(), vertexData);
	EXPECT_EQ(drained->fragmentSpv.data(), fragmentData);

	// Contents are what we put in.
	EXPECT_EQ(drained->vertexSpv.front(), 0xABu);
	EXPECT_EQ(drained->fragmentSpv.front(), 0xCDu);
}

// ---------------------------------------------------------------------------
// 7. Move-only payload: RenderRequestQueue is now BatchQueue<RenderRequest>, so
//    the same template must carry a strictly move-only item (no copy ctor at
//    all) through Push + TakeAll, FIFO, leaving the queue empty. std::unique_ptr
//    is the canonical move-only type; a queue that copied anywhere would fail to
//    compile, and one that reordered would fail the value checks.
// ---------------------------------------------------------------------------

TEST(RenderRequestQueueTest, MoveOnlyItemTypeRoundTripsFifo)
{
	BatchQueue<std::unique_ptr<int>> queue;
	EXPECT_TRUE(queue.Empty());

	queue.Push(std::make_unique<int>(1));
	queue.Push(std::make_unique<int>(2));
	queue.Push(std::make_unique<int>(3));
	EXPECT_FALSE(queue.Empty());

	auto batch = queue.TakeAll();
	ASSERT_EQ(batch.size(), 3u);

	// FIFO: push order is drain order, and each pointer still owns its value.
	ASSERT_NE(batch[0], nullptr);
	ASSERT_NE(batch[1], nullptr);
	ASSERT_NE(batch[2], nullptr);
	EXPECT_EQ(*batch[0], 1);
	EXPECT_EQ(*batch[1], 2);
	EXPECT_EQ(*batch[2], 3);

	// The storage left with the batch: the queue is empty again.
	EXPECT_TRUE(queue.Empty());
}
