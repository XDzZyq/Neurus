/**
 * @file test_gizmo_pass.cpp
 * @brief GPU verification of GizmoPass: rasterization, depth-freedom, draw accounting.
 *
 * GizmoPass is the transform handle's renderer half, and it is deliberately the
 * *simpler* sibling of DebugPass: no depth attachment at all, no x-ray partition,
 * no wire pipeline. Every test below isolates one of those three claims, plus the
 * two payload boundaries (null vs empty) that decide whether the pass touches the
 * image at all.
 *
 * The sharpest test is `AxisLine_DrawsThroughNearDepth`. It submits the *same*
 * geometry, against the *same* camera, with the *same* near-primed depth buffer as
 * `DebugPassTest.DepthTestedLine_OccludedByNearDepth` — and expects the opposite
 * result. There, the line is rejected by the depth test; here it must be drawn,
 * because the render pass declares no depth attachment and the flags carry no
 * `XRay` bit to compensate. A regression that quietly attached the G-Buffer depth
 * would fail exactly this test and nothing else.
 *
 * Geometry is analytically predictable rather than eyeballed, and identical to the
 * DebugPass fixture so the two files stay comparable: the camera sits at (0,-5,0)
 * looking at the origin along +Y with up = +Z, so a segment along X through the
 * origin projects to a horizontal band across the middle of a square target.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestReferenceImage.h"

#include "asset/data/ImageData.h"
#include "render/Barrier.h"
#include "render/Image.h"
#include "render/RenderCache.h"
#include "render/RenderConfig.h"
#include "render/RenderContext.h"
#include "render/passes/GizmoPass.h"
#include "render/resources/GizmoCache.h"
#include "scene/Camera.h"
#include "scene/GizmoDrawList.h"
#include "scene/OverlayGeometry.h"
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
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
 * @brief Headless fixture: a RenderCache and a GizmoPass, no swapchain.
 *
 * GizmoPass draws into a RenderCache attachment with LOAD_OP_LOAD, which a test can
 * set up by hand — so no surface, renderer or graph is needed and this runs on every
 * platform, including macOS where the presentation-based tests are skipped.
 */
class GizmoPassTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;
		m_cache = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
		m_pass  = std::make_unique<GizmoPass>(*m_device, PhysicalDevice(), 2);

		m_camera = std::make_shared<Camera>();
		m_camera->SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));
		m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		m_camera->ChangeCamRatio(static_cast<float>(kRes), static_cast<float>(kRes));
		m_scene.UseCamera(m_camera);
		m_scene.ActivateCamera(m_camera->GetObjectID());
	}

	void TearDown() override
	{
		m_pass.reset();
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	/**
	 * @brief Primes the target color image and the depth image the pass must ignore.
	 *
	 * The depth argument exists precisely because GizmoPass has no depth attachment:
	 * priming it to 0.0 (everything solid, directly in front of the camera) is what
	 * makes "drawn anyway" a meaningful assertion rather than a tautology.
	 *
	 * @param depth  Value written to every depth texel of AttachmentName::Depth.
	 * @param color  Which post-chain color image to prime. ComposedOutput is
	 *               GizmoPass's default target; FXAAOutput is the one it draws into
	 *               when FXAAPass runs ahead of it.
	 * @param clear  Value written to every color texel, so a LOAD_OP_LOAD test can
	 *               start from something other than black.
	 */
	void PrimeAttachments(float depth,
	                      AttachmentName color = AttachmentName::ComposedOutput,
	                      glm::vec4 clear = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f))
	{
		auto& colorAtt = m_cache->GetAttachment(color, Extent());
		auto& dep      = m_cache->GetAttachment(AttachmentName::Depth, Extent());

		auto& c = BeginCmd();
		Barrier::Transition(*c, colorAtt, ImageState::TransferDst);
		Barrier::Transition(*c, dep, ImageState::TransferDst);

		const vk::ImageSubresourceRange colorRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
		const vk::ImageSubresourceRange depthRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1);

		c.clearColorImage(*colorAtt.ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
		                  vk::ClearColorValue(std::array<float, 4>{clear.r, clear.g,
		                                                          clear.b, clear.a}),
		                  colorRange);
		c.clearDepthStencilImage(*dep.ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
		                         vk::ClearDepthStencilValue(depth, 0), depthRange);
		EndSubmitWait(c);
	}

	/**
	 * @brief Records the pass over the primed attachments and waits for it.
	 *
	 * Takes the payload by *pointer* on purpose: a null pointer is the real signal
	 * for "no gesture in progress" (GizmoDrawList.h), and it is a distinct code path
	 * from an empty list. The cache update is guarded the same way
	 * DeferredRenderer::recordFrame() guards it, so a null payload leaves GizmoCache
	 * untouched — which is what lets a test prove the early-out happens before the
	 * cache is ever consulted.
	 *
	 * The pass itself uploads nothing: the camera UBO and the two gizmo SSBOs belong
	 * to RenderCache and the frame driver writes them before any pass records. This
	 * fixture stands in for that driver, so it must do the same or GizmoPass finds an
	 * invalid CameraGPU and null buffers.
	 */
	PassStats RunPass(const GizmoDrawList* list)
	{
		RenderContext ctx;
		ctx.width = kRes;
		ctx.height = kRes;
		ctx.frameIndex = 0;
		ctx.editor.config = &m_config;
		ctx.editor.scene = &m_scene;
		ctx.editor.gizmoDraw = list;

		PublishSceneCamera(*m_cache, m_scene);
		if (list) m_cache->UpdateGizmoDraw(ctx.frameIndex, *list);

		auto& c = BeginCmd();
		PassStats stats = m_pass->Record(*c, *m_cache, ctx);
		EndSubmitWait(c);
		return stats;
	}

	/**
	 * @brief Reads a color attachment back as float RGB, one entry per pixel.
	 *
	 * The attachments are half-float, so every other reader here goes through this
	 * one conversion rather than repeating it.
	 */
	std::vector<glm::vec3> ReadRGB(AttachmentName color = AttachmentName::ComposedOutput)
	{
		auto& colorAtt = m_cache->GetAttachment(color, Extent());
		auto data = colorAtt.ReadImageData(*m_device, PhysicalDevice(),
		                                   m_queue, m_graphicsQueueFamily);
		const auto* h = reinterpret_cast<const uint16_t*>(data->GetPixelData().data());

		std::vector<glm::vec3> rgb(static_cast<size_t>(kRes) * kRes);
		for (size_t p = 0; p < rgb.size(); ++p)
		{
			const size_t i = p * 4;
			rgb[p] = glm::vec3(VulkanTestShared::HalfToFloat(h[i + 0]),
			                   VulkanTestShared::HalfToFloat(h[i + 1]),
			                   VulkanTestShared::HalfToFloat(h[i + 2]));
		}
		return rgb;
	}

	/**
	 * @brief Measures where a color attachment is no longer black.
	 *
	 * The threshold is deliberately low (luma > 0.1): guides are drawn with alpha-over
	 * blending, so edge pixels are partial coverage, and the question these tests ask
	 * is "did anything land here at all", not "how bright".
	 *
	 * @param u8Out Optional receiver for an 8-bit RGBA copy, for the PNG dump.
	 * @param color Which image to read; defaults to GizmoPass's default target.
	 */
	LitStats Measure(std::vector<uint8_t>* u8Out = nullptr,
	                 AttachmentName color = AttachmentName::ComposedOutput)
	{
		const std::vector<glm::vec3> rgb = ReadRGB(color);
		if (u8Out) u8Out->resize(static_cast<size_t>(kRes) * kRes * 4);

		const auto to8 = [](float v) {
			return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
		};

		LitStats s;
		for (uint32_t y = 0; y < kRes; ++y)
		for (uint32_t x = 0; x < kRes; ++x)
		{
			const size_t p = static_cast<size_t>(y) * kRes + x;
			const glm::vec3& c = rgb[p];

			if (u8Out)
			{
				(*u8Out)[p * 4 + 0] = to8(c.r);
				(*u8Out)[p * 4 + 1] = to8(c.g);
				(*u8Out)[p * 4 + 2] = to8(c.b);
				(*u8Out)[p * 4 + 3] = 255u;
			}

			if (0.299f * c.r + 0.587f * c.g + 0.114f * c.b > 0.1f)
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

	vk::Extent2D Extent() const { return vk::Extent2D{kRes, kRes}; }

	std::unique_ptr<RenderCache> m_cache;
	std::unique_ptr<GizmoPass>   m_pass;
	Scene                        m_scene;
	std::shared_ptr<Camera>      m_camera;
	RenderConfig                 m_config;
};

} // namespace

// ---------------------------------------------------------------------------
// Shared geometry
// ---------------------------------------------------------------------------

namespace {

/// @brief A white pixel-wide segment through the world origin, spanning x in [-2,2].
OverlaySegment AxisSegment(float widthPx = 5.0f,
                           glm::vec4 color = glm::vec4(1.0f),
                           uint32_t flags = OverlayFlag::None)
{
	OverlaySegment seg;
	seg.a = glm::vec3(-2.0f, 0.0f, 0.0f);
	seg.b = glm::vec3(2.0f, 0.0f, 0.0f);
	seg.width = widthPx;
	seg.rgba = PackOverlayColor(color);
	seg.flags = flags;
	return seg;
}

/// @brief A screen-space square sprite at the world origin = the target's centre.
OverlayPointSprite PivotPoint(float sizePx = 12.0f, glm::vec4 color = glm::vec4(1.0f))
{
	OverlayPointSprite pt;
	pt.p = glm::vec3(0.0f);
	pt.size = sizePx;
	pt.rgba = PackOverlayColor(color);
	pt.shape = OverlayPointShape::Square;
	pt.flags = OverlayFlag::ScreenSpaceSize;
	return pt;
}

/**
 * @brief The one axis line, alone — the minimal list the pass will draw.
 *
 * No `XRay` flag and no partition index: GizmoDrawList has neither, which is half
 * the point of the depth test below.
 */
GizmoDrawList MakeAxisList()
{
	GizmoDrawList list;
	list.segments.push_back(AxisSegment());
	return list;
}

/// @brief Middle rows a 5 px line through the origin is allowed to occupy.
constexpr uint32_t kBandLo = kRes / 2 - 6;
constexpr uint32_t kBandHi = kRes / 2 + 5;

} // namespace

// ===========================================================================
// 1. No gesture: a null payload must not touch the image *or* the cache
// ===========================================================================

/**
 * @test A null GizmoDrawList early-returns before the cache is consulted.
 *
 * Null is the real "no modal gesture in progress" signal, and it is the state the
 * pass sits in for almost every frame of the app's life — so it has to be free. The
 * cache assertion is what makes this stronger than a lit-pixel count: GizmoCache
 * allocates its first ring slot inside Update(), so a frame count of zero proves no
 * upload, no allocation and no descriptor write happened at all.
 */
TEST_F(GizmoPassTest, NullPayload_LeavesImageAndCacheUntouched)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);

	const PassStats stats = RunPass(nullptr);
	const LitStats lit = Measure();

	std::cout << "[GizmoPass] null: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " cacheSlots=" << m_cache->GetGizmoCache().GetFrameCount() << std::endl;

	EXPECT_EQ(stats.drawCalls, 0u) << "No gesture must issue no draws";
	EXPECT_EQ(lit.count, 0) << "No gesture must leave the shaded image alone";
	EXPECT_EQ(m_cache->GetGizmoCache().GetFrameCount(), 0u)
		<< "A null payload must not even allocate a ring slot";
}

// ===========================================================================
// 2. An empty list is uploaded, draws nothing, and zeroes the counts
// ===========================================================================

/**
 * @test An empty (but non-null) list still updates the cache, and still draws nothing.
 *
 * This is the frame a gesture *ends* on: the list is cleared rather than detached, and
 * the zeroed counts are what make the previous frame's guide disappear. A cache that
 * skipped the update here would leave stale geometry resident and keep drawing it.
 */
TEST_F(GizmoPassTest, EmptyList_DrawsNothingAndZeroesTheCounts)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);

	// A real gesture first, so the slot holds geometry that must then be retired.
	const GizmoDrawList live = MakeAxisList();
	ASSERT_EQ(RunPass(&live).drawCalls, 1u) << "Precondition: the guide draws at all";

	GizmoDrawList empty;
	ASSERT_TRUE(empty.Empty());
	PrimeAttachments(1.0f);   // wipe the guide the precondition just drew
	const PassStats stats = RunPass(&empty);
	const LitStats lit = Measure();

	const GizmoCache::FrameView frame = m_cache->GetGizmoCache().GetFrame(0);

	std::cout << "[GizmoPass] empty: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " segs=" << frame.segmentCount
	          << " pts=" << frame.pointCount << std::endl;

	EXPECT_EQ(stats.drawCalls, 0u) << "An empty list must issue no draws";
	EXPECT_EQ(lit.count, 0) << "An empty list must leave the shaded image alone";
	EXPECT_EQ(frame.segmentCount, 0u) << "The previous frame's segments were not retired";
	EXPECT_EQ(frame.pointCount, 0u) << "The previous frame's points were not retired";
}

// ===========================================================================
// 3. No depth attachment: the guide draws through solid near geometry
// ===========================================================================

/**
 * @test The axis line is drawn against a fully-near depth buffer, with no XRay flag.
 *
 * Same segment, same camera, same depth prime as
 * `DebugPassTest.DepthTestedLine_OccludedByNearDepth`, which expects zero lit pixels.
 * The opposite expectation here is the entire depth claim of the pass: GizmoPass
 * binds no depth attachment, so there is nothing to test against and nothing to
 * bypass. Note the flags are `None` — this is *not* the x-ray path being exercised
 * under another name, because GizmoDrawList carries no x-ray partition to begin with.
 */
TEST_F(GizmoPassTest, AxisLine_DrawsThroughNearDepth)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(0.0f);   // solid geometry directly in front of the camera

	const GizmoDrawList list = MakeAxisList();
	const PassStats stats = RunPass(&list);

	std::vector<uint8_t> rgba;
	const LitStats lit = Measure(&rgba);

	std::cout << "[GizmoPass] axis line: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " rows=[" << lit.minRow << "," << lit.maxRow << "]"
	          << " cols=[" << lit.minCol << "," << lit.maxCol << "]" << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u) << "Segments alone = one draw";

	// A 5 px line spanning most of the width: at least 4 rows x 40 columns.
	EXPECT_GT(lit.count, 160)
		<< "The guide was occluded — GizmoPass acquired a depth attachment";

	// Horizontal: the band must stay near the vertical middle, and must be wide.
	EXPECT_GE(lit.minRow, kBandLo) << "Line drifted above the expected band";
	EXPECT_LE(lit.maxRow, kBandHi) << "Line drifted below the expected band";
	EXPECT_LE(lit.maxRow - lit.minRow, 8u) << "Band is thicker than a 5 px line";
	EXPECT_GE(lit.maxCol - lit.minCol, kRes / 2)
		<< "Line does not span at least half the width";

	// Reference-image regression on top of the analytical checks above.
	{
		const std::string refPath = neurus::test::ReferencePath::Make("gizmo/GizmoAxisLine.png");
		std::filesystem::create_directories(std::filesystem::path(refPath).parent_path());
		ImageData img(rgba.data(), kRes, kRes, PixelFormat::RGBA8U);
		ASSERT_TRUE(img.SavePNG(refPath + ".tmp")) << "Failed to save gizmo line PNG";
		const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 4);
		if (refResult < 0)
			GTEST_SKIP() << "Reference image generated. Re-run the test to compare.";
		else
			EXPECT_EQ(refResult, 0) << refResult << " pixel(s) differ from reference";
	}
}

// ===========================================================================
// 4. Draw accounting: two draws for any payload, however large
// ===========================================================================

/**
 * @test A 49-segment rotation guide plus its pivot dot costs exactly two draws.
 *
 * 49 segments and 1 point is the real constrained-Rotate payload
 * (`GizmoDrawBuilder`: one axis line + 48 arc chords + the pivot). DebugPass would
 * spend up to six draws on the same geometry because it splits each primitive kind
 * across the x-ray partition; GizmoPass has no partition, so the count is one draw
 * per *pipeline* and nothing else. Mixed flags are set deliberately: an `XRay` bit in
 * the payload must not cause a split, because nothing reads it here.
 */
TEST_F(GizmoPassTest, ManySegmentsAndPoints_AreExactlyTwoDraws)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);

	GizmoDrawList list;
	for (int i = 0; i < 49; ++i)
	{
		const float t = static_cast<float>(i) / 48.0f;
		OverlaySegment seg = AxisSegment(3.0f, glm::vec4(1.0f),
		                                 (i % 2) ? OverlayFlag::XRay : OverlayFlag::None);
		seg.a.z = -1.0f + 2.0f * t;   // fan the chords vertically so they are separable
		seg.b.z = seg.a.z;
		list.segments.push_back(seg);
	}
	list.points.push_back(PivotPoint());

	const PassStats stats = RunPass(&list);
	const LitStats lit = Measure();

	const GizmoCache::FrameView frame = m_cache->GetGizmoCache().GetFrame(0);

	std::cout << "[GizmoPass] 49+1: draws=" << stats.drawCalls
	          << " lit=" << lit.count
	          << " segs=" << frame.segmentCount
	          << " pts=" << frame.pointCount << std::endl;

	EXPECT_EQ(stats.drawCalls, 2u)
		<< "One draw per pipeline, regardless of primitive count or XRay bits";
	EXPECT_EQ(frame.segmentCount, 49u) << "Every segment must reach the SSBO";
	EXPECT_EQ(frame.pointCount, 1u) << "The pivot point must reach the SSBO";
	EXPECT_GT(lit.count, 400) << "A 49-chord guide barely rasterized";
}

// ===========================================================================
// 5. Draw order: the pivot dot lands on top of the axis line
// ===========================================================================

/**
 * @test A white pivot sprite covers the red axis line it overlaps, not the reverse.
 *
 * Depth is off and blending is alpha-over, so submission order is the *only* thing
 * deciding what is visible where two guides overlap — which is why GizmoPass submits
 * segments first and points second, and why GizmoDrawList records the same contract on
 * the producing side. Two saturated, complementary colors make the winner readable from
 * one channel: the centre must be white (the sprite), the line's far end still red.
 */
TEST_F(GizmoPassTest, PivotPoint_DrawsOverTheAxisLine)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f);

	GizmoDrawList list;
	list.segments.push_back(AxisSegment(9.0f, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
	list.points.push_back(PivotPoint(12.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)));

	const PassStats stats = RunPass(&list);
	const std::vector<glm::vec3> rgb = ReadRGB();

	const auto at = [&](uint32_t x, uint32_t y) {
		return rgb[static_cast<size_t>(y) * kRes + x];
	};

	// Mean green over a 4x4 block at the centre: green is the sprite's signature,
	// since the line carries none. A block rather than one texel, because the origin
	// projects to the *boundary* between two pixels.
	float centreGreen = 0.0f;
	for (uint32_t y = 30; y < 34; ++y)
	for (uint32_t x = 30; x < 34; ++x)
		centreGreen += at(x, y).g;
	centreGreen /= 16.0f;

	// The line's far end, well outside a 12 px sprite: still pure red.
	float endRed = 0.0f, endGreen = 0.0f;
	for (uint32_t x = 4; x < 9; ++x)
	{
		endRed   += at(x, kRes / 2).r;
		endGreen += at(x, kRes / 2).g;
	}
	endRed /= 5.0f;
	endGreen /= 5.0f;

	std::cout << "[GizmoPass] order: draws=" << stats.drawCalls
	          << " centreGreen=" << centreGreen
	          << " endRed=" << endRed
	          << " endGreen=" << endGreen << std::endl;

	EXPECT_EQ(stats.drawCalls, 2u) << "Segments and points = two draws";
	EXPECT_GT(centreGreen, 0.5f)
		<< "The axis line covered the pivot dot — points must be submitted last";
	EXPECT_GT(endRed, 0.5f) << "The axis line did not rasterize at all";
	EXPECT_LT(endGreen, 0.1f) << "The sprite is far wider than its 12 px size";
}

// ===========================================================================
// 6. LOAD_OP_LOAD: the shaded image survives underneath the guide
// ===========================================================================

/**
 * @test Pixels the guide does not cover keep the value the previous pass left there.
 *
 * GizmoPass is the frame's last writer and draws into the *finished* image, so a
 * LOAD_OP_CLEAR would not merely look wrong, it would discard the whole render. A
 * black prime cannot catch that — cleared and loaded black are the same pixels — so
 * the target starts at a dark grey the guide will not overwrite.
 */
TEST_F(GizmoPassTest, LoadOpLoad_PreservesTheShadedImage)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	constexpr float kGrey = 0.25f;
	PrimeAttachments(1.0f, AttachmentName::ComposedOutput,
	                 glm::vec4(kGrey, kGrey, kGrey, 1.0f));

	const GizmoDrawList list = MakeAxisList();
	const PassStats stats = RunPass(&list);
	const std::vector<glm::vec3> rgb = ReadRGB();

	const auto at = [&](uint32_t x, uint32_t y) {
		return rgb[static_cast<size_t>(y) * kRes + x];
	};

	// Two corners, far from a band through the middle.
	const glm::vec3 topLeft = at(1, 1);
	const glm::vec3 botRight = at(kRes - 2, kRes - 2);
	const glm::vec3 onLine = at(kRes / 2, kRes / 2);

	std::cout << "[GizmoPass] loadop: draws=" << stats.drawCalls
	          << " corner=" << topLeft.r
	          << " onLine=" << onLine.r << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u);
	EXPECT_NEAR(topLeft.r, kGrey, 0.01f) << "The image underneath was cleared, not loaded";
	EXPECT_NEAR(topLeft.g, kGrey, 0.01f);
	EXPECT_NEAR(botRight.r, kGrey, 0.01f) << "The image underneath was cleared, not loaded";
	EXPECT_GT(onLine.r, 0.9f) << "The white guide did not land on the centre row";
}

// ===========================================================================
// 7. Retarget: SetTarget() moves the guides to FXAA's output
// ===========================================================================

/**
 * @test With FXAA on, the guides must land in FXAAOutput and leave ComposedOutput alone.
 *
 * DeferredRenderer hands GizmoPass the same tail attachment DebugPass got, and then
 * blits `GizmoPass::GetTarget()` — so a pass that ignored SetTarget() would draw into
 * an image nobody presents. Asserting the *other* attachment stays black is the half
 * that catches a hardcoded target, which would otherwise still light a twin image.
 */
TEST_F(GizmoPassTest, SetTarget_RedirectsTheGuidesToFXAAOutput)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments(1.0f, AttachmentName::ComposedOutput);
	PrimeAttachments(1.0f, AttachmentName::FXAAOutput);

	m_pass->SetTarget(AttachmentName::FXAAOutput);
	EXPECT_EQ(m_pass->GetTarget(), AttachmentName::FXAAOutput)
		<< "GetTarget() is what DeferredRenderer blits from; it must report the target";

	const GizmoDrawList list = MakeAxisList();
	const PassStats stats = RunPass(&list);

	const LitStats onFxaa     = Measure(nullptr, AttachmentName::FXAAOutput);
	const LitStats onComposed = Measure(nullptr, AttachmentName::ComposedOutput);

	std::cout << "[GizmoPass] retarget: draws=" << stats.drawCalls
	          << " litFXAAOutput=" << onFxaa.count
	          << " litComposedOutput=" << onComposed.count
	          << " rows=[" << onFxaa.minRow << "," << onFxaa.maxRow << "]" << std::endl;

	EXPECT_EQ(stats.drawCalls, 1u) << "Segments alone = one draw";

	// Same line, same camera, same band as the ComposedOutput case: the two
	// attachments share format and usage, so one set of pipelines serves either.
	EXPECT_GT(onFxaa.count, 160) << "Guide did not rasterize into FXAAOutput";
	EXPECT_GE(onFxaa.minRow, kBandLo) << "Line drifted above the expected band";
	EXPECT_LE(onFxaa.maxRow, kBandHi) << "Line drifted below the expected band";
	EXPECT_GE(onFxaa.maxCol - onFxaa.minCol, kRes / 2)
		<< "Line does not span at least half the width";

	EXPECT_EQ(onComposed.count, 0)
		<< "The guide leaked into ComposedOutput, which the blit no longer reads";
}
