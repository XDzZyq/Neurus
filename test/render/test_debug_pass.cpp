/**
 * @file test_debug_pass.cpp
 * @brief GPU verification of DebugPass: rasterization, depth occlusion, x-ray.
 *
 * The four line tests all submit the *same* segment against the *same* camera and
 * differ in exactly one variable, so each acceptance criterion of issue #22 is
 * isolated instead of being inferred from one composite image:
 *
 *   depth = 1.0 (far),  depth-tested  -> drawn      (rasterization works)
 *   depth = 0.0 (near), depth-tested  -> NOT drawn  (depth occlusion works)
 *   depth = 0.0 (near), x-ray         -> drawn      (x-ray bypasses depth)
 *   empty list                        -> NOT drawn  (no-op leaves the image alone)
 *
 * Because the first and third produce the same pixels, "x-ray draws" cannot be
 * satisfied by a pass that simply ignores depth: the second test would fail.
 *
 * Geometry is chosen to be analytically predictable rather than eyeballed. The
 * camera sits at (0,-5,0) looking at the origin along +Y with up = +Z, so a
 * segment along X through the origin projects to a horizontal band across the
 * middle of a square target — its row extent is checked, not just its area.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestReferenceImage.h"

#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"
#include "render/Barrier.h"
#include "render/Image.h"
#include "render/RenderCache.h"
#include "render/RenderConfig.h"
#include "render/RenderContext.h"
#include "render/UploadManager.h"
#include "render/passes/DebugPass.h"
#include "render/resources/MeshGPU.h"
#include "scene/Camera.h"
#include "scene/DebugDrawList.h"
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

using namespace neurus;

namespace {

constexpr uint32_t kRes = 64;  ///< Square target: keeps the row extent symmetric.

/// @brief Where a lit pixel was found, so a band can be checked and not just a count.
struct LitStats
{
	int count = 0;
	uint32_t minRow = kRes;
	uint32_t maxRow = 0;
	uint32_t minCol = kRes;
	uint32_t maxCol = 0;
};

/**
 * @brief Headless fixture: a RenderCache and a DebugPass, no swapchain.
 *
 * DebugPass draws into RenderCache attachments with LOAD_OP_LOAD, which is
 * exactly what a test can set up by hand — so no surface, renderer or graph is
 * needed and this runs on every platform, including macOS where the
 * presentation-based SceneWiringTest tests are skipped.
 */
class DebugPassTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;
		m_cache = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
		m_pass  = std::make_unique<DebugPass>(*m_device, PhysicalDevice(), 2);

		m_camera = std::make_shared<Camera>();
		m_camera->SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));
		m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		m_camera->ChangeCamRatio(static_cast<float>(kRes), static_cast<float>(kRes));
		m_scene.UseCamera(m_camera);
	}

	void TearDown() override
	{
		m_pass.reset();
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	/**
	 * @brief Primes both attachments: black color, uniform depth.
	 * @param depth Value written to every depth texel. The pipeline compares with
	 *              eLess, so 1.0 lets debug geometry through and 0.0 occludes it.
	 */
	void PrimeAttachments(float depth)
	{
		auto& color = m_cache->GetAttachment(AttachmentName::ComposedOutput, Extent());
		auto& dep   = m_cache->GetAttachment(AttachmentName::Depth, Extent());

		auto& c = BeginCmd();
		Barrier::Transition(*c, color, ImageState::TransferDst);
		Barrier::Transition(*c, dep, ImageState::TransferDst);

		const vk::ImageSubresourceRange colorRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
		const vk::ImageSubresourceRange depthRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1);

		c.clearColorImage(*color.ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
		                  vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}),
		                  colorRange);
		c.clearDepthStencilImage(*dep.ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
		                         vk::ClearDepthStencilValue(depth, 0), depthRange);
		EndSubmitWait(c);
	}

	/** @brief Records the pass over the primed attachments and waits for it. */
	PassStats RunPass(const DebugDrawList& list)
	{
		RenderContext ctx;
		ctx.width = kRes;
		ctx.height = kRes;
		ctx.frameIndex = 0;
		ctx.editor.config = &m_config;
		ctx.editor.scene = &m_scene;
		ctx.editor.debugDraw = &list;

		auto& c = BeginCmd();
		PassStats stats = m_pass->Record(*c, *m_cache, ctx);
		EndSubmitWait(c);
		return stats;
	}

	vk::Extent2D Extent() const { return vk::Extent2D{kRes, kRes}; }

	/**
	 * @brief Reads ComposedOutput back and measures where it is no longer black.
	 *
	 * The threshold is deliberately low (luma > 0.1): a debug line is drawn with
	 * alpha-over blending, so its edge pixels are partial coverage, and the
	 * question these tests ask is "did anything land here at all", not "how bright".
	 *
	 * @param u8Out Optional receiver for an 8-bit RGBA copy, for the PNG dump.
	 */
	LitStats Measure(std::vector<uint8_t>* u8Out = nullptr)
	{
		auto& color = m_cache->GetAttachment(AttachmentName::ComposedOutput, Extent());
		auto data = color.ReadImageData(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
		const auto* h = reinterpret_cast<const uint16_t*>(data->GetPixelData().data());

		if (u8Out) u8Out->resize(static_cast<size_t>(kRes) * kRes * 4);

		LitStats s;
		for (uint32_t y = 0; y < kRes; ++y)
		for (uint32_t x = 0; x < kRes; ++x)
		{
			const size_t i = (static_cast<size_t>(y) * kRes + x) * 4;
			const float r = VulkanTestShared::HalfToFloat(h[i + 0]);
			const float g = VulkanTestShared::HalfToFloat(h[i + 1]);
			const float b = VulkanTestShared::HalfToFloat(h[i + 2]);

			if (u8Out)
			{
				const auto to8 = [](float v) {
					return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
				};
				(*u8Out)[i + 0] = to8(r);
				(*u8Out)[i + 1] = to8(g);
				(*u8Out)[i + 2] = to8(b);
				(*u8Out)[i + 3] = 255u;
			}


			if (0.299f * r + 0.587f * g + 0.114f * b > 0.1f)
			{
				++s.count;
				s.minRow = std::min(s.minRow, y);
				s.maxRow = std::max(s.maxRow, y);
				s.minCol = std::min(s.minCol, x);
				s.maxCol = std::max(s.maxCol, x);
			}
		}
		return s;
	}

	std::unique_ptr<RenderCache> m_cache;
	std::unique_ptr<DebugPass>   m_pass;
	Scene                        m_scene;
	std::shared_ptr<Camera>      m_camera;
	RenderConfig                 m_config;
};

} // namespace

// ---------------------------------------------------------------------------
// Shared geometry
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief One horizontal white segment through the world origin.
 *
 * Spans x in [-2, 2] at y = z = 0, so with the fixture camera it crosses the
 * whole width of the target through its vertical middle. 5 px wide: thick enough
 * that a few rows are fully covered, thin enough that the band is a real
 * constraint on the projection.
 */
DebugDrawList MakeLineList(bool xray)
{
	DebugDrawList list;

	DebugSegment seg;
	seg.a = glm::vec3(-2.0f, 0.0f, 0.0f);
	seg.b = glm::vec3(2.0f, 0.0f, 0.0f);
	seg.width = 5.0f;
	seg.rgba = PackDebugColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
	seg.flags = xray ? DebugFlag::XRay : DebugFlag::None;
	list.segments.push_back(seg);

	// The partition boundary is what DebugPass draws from: an x-ray segment must
	// sit at or after xraySegmentStart, a depth-tested one before it.
	list.xraySegmentStart = xray ? 0u : 1u;
	list.Touch();
	return list;
}

/// @brief Middle rows a 5 px line through the origin is allowed to occupy.
constexpr uint32_t kBandLo = kRes / 2 - 6;
constexpr uint32_t kBandHi = kRes / 2 + 5;

} // namespace

// ===========================================================================
// 1. No-op: an empty list must not touch the image
// ===========================================================================

TEST_F(DebugPassTest, EmptyList_LeavesImageUntouched)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);

	DebugDrawList empty;
	const PassStats stats = RunPass(empty);
	const LitStats lit = Measure();

	std::cout << "[DebugPass] empty: draws=" << stats.drawCalls
	          << " lit=" << lit.count << std::endl;

	EXPECT_EQ(stats.drawCalls, 0u) << "An empty list must issue no draws";
	EXPECT_EQ(lit.count, 0) << "An empty list must leave the composed image black";
}

// ===========================================================================
// 2. Rasterization: a depth-tested line against far depth is drawn
// ===========================================================================

TEST_F(DebugPassTest, DepthTestedLine_VisibleAgainstFarDepth)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);   // nothing in front of the overlay

	const DebugDrawList list = MakeLineList(/*xray=*/false);
	const PassStats stats = RunPass(list);

	std::vector<uint8_t> rgba;
	const LitStats lit = Measure(&rgba);

	std::cout << "[DebugPass] visible: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " rows=[" << lit.minRow << "," << lit.maxRow << "]"
	          << " cols=[" << lit.minCol << "," << lit.maxCol << "]" << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u) << "One depth-tested range = one draw";

	// A 5 px line spanning most of the width: at least 4 rows x 40 columns.
	EXPECT_GT(lit.count, 160) << "Line did not rasterize (or is far too thin)";

	// Horizontal: the band must stay near the vertical middle, and must be wide.
	EXPECT_GE(lit.minRow, kBandLo) << "Line drifted above the expected band";
	EXPECT_LE(lit.maxRow, kBandHi) << "Line drifted below the expected band";
	EXPECT_LE(lit.maxRow - lit.minRow, 8u) << "Band is thicker than a 5 px line";
	EXPECT_GE(lit.maxCol - lit.minCol, kRes / 2)
		<< "Line does not span at least half the width";

	// Reference-image regression on top of the analytical checks above.
	{
		const std::string refPath = neurus::test::ReferencePath::Make("debug/DebugLine_Visible.png");
		ImageData img(rgba.data(), kRes, kRes, PixelFormat::RGBA8U);
		ASSERT_TRUE(img.SavePNG(refPath + ".tmp")) << "Failed to save debug line PNG";
		const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 4);
		if (refResult < 0)
			GTEST_SKIP() << "Reference image generated. Re-run the test to compare.";
		else
			EXPECT_EQ(refResult, 0) << refResult << " pixel(s) differ from reference";
	}
}

// ===========================================================================
// 3. Depth occlusion: the same line behind near depth is hidden
// ===========================================================================

TEST_F(DebugPassTest, DepthTestedLine_OccludedByNearDepth)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(0.0f);   // solid geometry directly in front of the camera

	const DebugDrawList list = MakeLineList(/*xray=*/false);
	const PassStats stats = RunPass(list);
	const LitStats lit = Measure();

	std::cout << "[DebugPass] occluded: draws=" << stats.drawCalls
	          << " lit=" << lit.count << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u) << "The draw is still issued; depth rejects it";
	EXPECT_EQ(lit.count, 0)
		<< lit.count << " pixel(s) survived a full-near depth buffer — "
		   "the overlay is not depth-tested";
}

// ===========================================================================
// 4. X-ray: the same line, same near depth, drawn anyway
// ===========================================================================

TEST_F(DebugPassTest, XRayLine_DrawsThroughNearDepth)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(0.0f);   // identical to the occlusion test above

	const DebugDrawList list = MakeLineList(/*xray=*/true);
	const PassStats stats = RunPass(list);
	const LitStats lit = Measure();

	std::cout << "[DebugPass] xray: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " rows=[" << lit.minRow << "," << lit.maxRow << "]" << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u) << "One x-ray range = one draw";
	EXPECT_GT(lit.count, 160)
		<< "X-ray line was occluded; eDepthTestEnable was not disabled";
	EXPECT_GE(lit.minRow, kBandLo);
	EXPECT_LE(lit.maxRow, kBandHi);
}

// ===========================================================================
// 5. Point sprites: screen-space size lands where the point projects
// ===========================================================================

TEST_F(DebugPassTest, ScreenSpacePoint_DrawsSquareAtProjectedPosition)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);

	DebugDrawList list;
	DebugPointSprite p;
	p.p = glm::vec3(0.0f, 0.0f, 0.0f);          // the origin = screen centre
	p.size = 12.0f;                              // pixels, per ScreenSpaceSize
	p.rgba = PackDebugColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
	p.shape = DebugPointShape::Square;
	p.flags = DebugFlag::ScreenSpaceSize;
	list.points.push_back(p);
	list.xrayPointStart = 1u;
	list.Touch();

	const PassStats stats = RunPass(list);
	const LitStats lit = Measure();

	std::cout << "[DebugPass] point: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " rows=[" << lit.minRow << "," << lit.maxRow << "]"
	          << " cols=[" << lit.minCol << "," << lit.maxCol << "]" << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u);

	// A square sprite of s px covers s*s pixels; allow for a device that clamps
	// gl_PointSize below 12 (pointSizeRange is queried and pushed by the pass).
	EXPECT_GT(lit.count, 16) << "Point sprite did not rasterize";

	// It must be centred: the sprite's bounding box has to contain the middle.
	EXPECT_LE(lit.minRow, kRes / 2) << "Sprite is entirely below centre";
	EXPECT_GE(lit.maxRow, kRes / 2 - 1) << "Sprite is entirely above centre";
	EXPECT_LE(lit.minCol, kRes / 2) << "Sprite is entirely right of centre";
	EXPECT_GE(lit.maxCol, kRes / 2 - 1) << "Sprite is entirely left of centre";

	// And square-ish, not smeared across the target.
	EXPECT_LE(lit.maxRow - lit.minRow, 20u) << "Sprite taller than its size";
	EXPECT_LE(lit.maxCol - lit.minCol, 20u) << "Sprite wider than its size";
}

// ===========================================================================
// 6. Budget: issue #22 asks for 10,000 debug lines in under 0.5 ms
// ===========================================================================

/**
 * @test 10,000 segments cost near-nothing more than one segment.
 *
 * The absolute wall clock of a record-submit-wait round trip is dominated by
 * fixed submission and fence latency, which has nothing to do with the overlay,
 * so a bare timing of the 10k case would measure the driver rather than the pass.
 * This measures the *marginal* cost instead: the same round trip is timed with 1
 * segment and with 10,000, and the difference is what 9,999 extra lines cost.
 *
 * The structural half of the budget matters just as much and is asserted exactly:
 * 10,000 segments must still be two draw calls, because the list is partitioned
 * by depth mode rather than sorted or drawn per primitive.
 */
TEST_F(DebugPassTest, TenThousandSegments_StayWithinBudget)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	constexpr uint32_t kCount = 10000;
	ASSERT_LE(kCount, DebugPass::kMaxSegments) << "Test exceeds the pass capacity";

	// A fan of segments through the origin, half depth-tested and half x-ray, so
	// both halves of the partition are exercised in one frame.
	DebugDrawList big;
	big.segments.reserve(kCount);
	const uint32_t half = kCount / 2;
	for (uint32_t i = 0; i < kCount; ++i)
	{
		const float t = static_cast<float>(i) / static_cast<float>(kCount);
		const float ang = t * 6.2831853f;
		DebugSegment seg;
		seg.a = glm::vec3(0.0f, 0.0f, 0.0f);
		seg.b = glm::vec3(std::cos(ang) * 2.0f, 0.0f, std::sin(ang) * 2.0f);
		seg.width = 1.0f;
		seg.rgba = PackDebugColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
		// The depth-tested half must come first: that is the partition contract.
		seg.flags = (i < half) ? DebugFlag::None : DebugFlag::XRay;
		big.segments.push_back(seg);
	}
	big.xraySegmentStart = half;
	big.Touch();

	const DebugDrawList one = MakeLineList(/*xray=*/false);

	// Warm up: first submission pays for pipeline/descriptor lazy work.
	PrimeAttachments(1.0f);
	RunPass(big);

	const auto timeIt = [this](const DebugDrawList& list) {
		PrimeAttachments(1.0f);
		const auto t0 = std::chrono::steady_clock::now();
		const PassStats s = RunPass(list);
		const auto t1 = std::chrono::steady_clock::now();
		return std::pair<PassStats, double>{
			s, std::chrono::duration<double, std::milli>(t1 - t0).count()};
	};

	// Best of several runs on each: the minimum is the least scheduler-polluted
	// sample, and a budget question is about the achievable cost, not the median.
	double bigMs = 1e9, oneMs = 1e9;
	PassStats bigStats{};
	for (int rep = 0; rep < 5; ++rep)
	{
		const auto b = timeIt(big);
		const auto o = timeIt(one);
		bigStats = b.first;
		bigMs = std::min(bigMs, b.second);
		oneMs = std::min(oneMs, o.second);
	}

	const double marginalMs = bigMs - oneMs;
	std::cout << "[DebugPass] 10k segments: draws=" << bigStats.drawCalls
	          << " big=" << bigMs << "ms one=" << oneMs << "ms"
	          << " marginal=" << marginalMs << "ms" << std::endl;

	// Structural budget: two draws, independent of primitive count.
	EXPECT_EQ(bigStats.drawCalls, 2u)
		<< "10,000 segments must collapse into one depth-tested and one x-ray draw";

	// Timing budget from issue #22. The marginal cost can measure slightly
	// negative when the two round trips land in different scheduler slots, which
	// is a pass, not an anomaly.
	EXPECT_LT(marginalMs, 0.5)
		<< "9,999 extra segments cost " << marginalMs << " ms (budget 0.5 ms)";

	// It has to actually be on screen, or the timing is measuring nothing.
	const LitStats lit = Measure();
	EXPECT_GT(lit.count, 100) << "The 10k fan did not rasterize";
}

// ===========================================================================
// 7. Wire meshes: the third primitive kind, drawn from a real MeshGPU
// ===========================================================================

namespace {

/**
 * @brief A 4x4 quad in the z = 0... plane facing the fixture camera, as OBJ text.
 *
 * Lies in the world XZ plane at y = 0, spanning [-2, 2] on both axes, so the
 * fixture camera at (0,-5,0) sees it face-on as a large square. Two triangles,
 * which under PolygonMode::eLine become the square's four edges plus the shared
 * diagonal — a shape that is unmistakably *not* a filled quad.
 */
constexpr const char* kQuadObj =
	"v -2 0 -2\n"
	"v 2 0 -2\n"
	"v 2 0 2\n"
	"v -2 0 2\n"
	"vn 0 -1 0\n"
	"vt 0 0\n"
	"f 1/1/1 2/1/1 3/1/1\n"
	"f 1/1/1 3/1/1 4/1/1\n";

/// @brief Arbitrary cache key: DebugWireMesh only needs an id that resolves.
constexpr int kWireObjectId = 4242;

} // namespace

/**
 * @test A DebugMesh wireframe rasterizes as edges, is depth-tested, and honours x-ray.
 *
 * All three assertions share one uploaded MeshGPU because building it is the
 * expensive part; they differ only in the primed depth and the x-ray flag, the
 * same one-variable discipline the line tests use.
 *
 * "It is a wireframe and not a filled quad" is asserted geometrically rather
 * than by eye: the lit pixels are counted against the area of their own bounding
 * box. A filled quad would light nearly all of it; five thin edges light a small
 * fraction. That distinction is the whole point of PolygonMode::eLine, so a
 * regression to eFill has to fail here.
 */
TEST_F(DebugPassTest, WireMesh_DrawsEdgesDepthTestedAndXRay)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	// --- Upload the quad through the same path Editor uses for a DebugMesh ---
	auto meshData = std::make_shared<MeshData>();
	ASSERT_TRUE(meshData->LoadObjFromString(kQuadObj)) << "Quad OBJ failed to parse";

	{
		UploadManager upload(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
		MeshGPU gpu = upload.UploadMeshData(*meshData);
		ASSERT_TRUE(gpu.vertexBuffer && gpu.indexBuffer) << "UploadMeshData produced no buffers";
		m_cache->UseMeshGPU(kWireObjectId, std::move(gpu));
	}
	ASSERT_NE(m_cache->GetMeshGPU(kWireObjectId), nullptr);

	const auto makeWireList = [](bool xray) {
		DebugDrawList list;
		DebugWireMesh wire;
		wire.model = glm::mat4(1.0f);
		wire.rgba = PackDebugColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
		wire.flags = xray ? DebugFlag::XRay : DebugFlag::None;
		wire.meshObjectId = kWireObjectId;
		list.wireMeshes.push_back(wire);
		list.Touch();
		return list;
	};

	// --- a) Far depth, depth-tested: the edges must appear ---
	PrimeAttachments(1.0f);
	const PassStats stats = RunPass(makeWireList(/*xray=*/false));
	EXPECT_EQ(stats.drawCalls, 1u) << "One wire mesh is one draw";

	const LitStats visible = Measure();
	ASSERT_GT(visible.count, 20) << "The wireframe did not rasterize at all";

	// It is a big shape: the quad should span a large part of a 64x64 target.
	EXPECT_GT(visible.maxCol - visible.minCol, kRes / 3)
		<< "Wireframe is too narrow to be the projected quad";
	EXPECT_GT(visible.maxRow - visible.minRow, kRes / 3)
		<< "Wireframe is too short to be the projected quad";

	// Edges, not a fill: lit pixels are a small fraction of their bounding box.
	const int boxArea = static_cast<int>((visible.maxCol - visible.minCol + 1) *
	                                     (visible.maxRow - visible.minRow + 1));
	std::cout << "[DebugPass] wireframe: lit=" << visible.count << " box=" << boxArea
	          << " rows=[" << visible.minRow << "," << visible.maxRow << "]"
	          << " cols=[" << visible.minCol << "," << visible.maxCol << "]" << std::endl;
	EXPECT_LT(visible.count, boxArea / 3)
		<< "Lit " << visible.count << " of " << boxArea
		<< " box pixels — that is a filled quad, not PolygonMode::eLine";

	// --- b) Near depth, depth-tested: the same wireframe must be occluded ---
	PrimeAttachments(0.0f);
	RunPass(makeWireList(/*xray=*/false));
	EXPECT_EQ(Measure().count, 0) << "Wireframe drew through a near depth buffer";

	// --- c) Near depth, x-ray: it must come back, proving the dynamic depth state ---
	PrimeAttachments(0.0f);
	RunPass(makeWireList(/*xray=*/true));
	const LitStats xray = Measure();
	EXPECT_GT(xray.count, 20) << "X-ray wireframe was still occluded";
	EXPECT_EQ(xray.count, visible.count)
		<< "X-ray must draw the same pixels as the unoccluded pass, not more or fewer";
}

