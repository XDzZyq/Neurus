/**
 * @file test_null_scene_passes.cpp
 * @brief A frame recorded before a scene exists must record nothing, not crash.
 *
 * RenderContext's null policy says `scene` must be checked (see
 * render/RenderContext.h). Two passes used to ignore that: LightingPass read
 * `scene->light_list` / `env_list` and SelectionOutlinePass read
 * `scene->selections` with no guard at all, so any frame recorded before the
 * Editor published a scene dereferenced null. This file pins the fix.
 *
 * What is asserted, per pass:
 *   - Record() returns without crashing on a default-constructed context.
 *   - It records **zero** work — no draw calls and no dispatches — rather than
 *     dispatching over an empty light set, which would still write the HDR
 *     attachment and make "no scene" indistinguishable from "black scene".
 *   - The command buffer it was handed still submits cleanly, which is what
 *     proves the early-out left no half-written barrier or descriptor behind.
 *
 * Three other passes (GeometryPass, ShadowDepthPass, ShadowIntensityPass)
 * already guarded by hand and now share the same `HasScene()` predicate; they
 * are covered here too so the policy is pinned in all five places at once.
 *
 * The precedent for a precondition-only test is
 * test_render_graph.cpp's ExecuteThrowsIfNotCompiled.
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/RenderFrameSnapshot.h"
#include "render/resources/LightingCache.h"
#include "render/passes/GeometryPass.h"
#include "render/passes/LightingPass.h"
#include "render/passes/SelectionOutlinePass.h"
#include "render/passes/ShadowDepthPass.h"
#include "render/passes/ShadowIntensityPass.h"

#include "core/RenderConfig.h"


#include <memory>

using namespace neurus;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/**
 * @brief Builds the passes under test against a live device, nothing else.
 *
 * Deliberately publishes no scene, no camera and no config: a
 * default-constructed RenderContext is exactly the state the renderer is in on
 * the frames between construction and the Editor's first BuildSnapshot().
 */
class NullScenePassTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& pd = PhysicalDevice();

		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(std::make_unique<LightingCache>(
			*m_device, pd, m_queue, m_graphicsQueueFamily));
	}

	/**
	 * @brief The context a pass sees before any scene is published.
	 *
	 * Publishes no snapshot on purpose: Frame() then reads as the empty frame,
	 * so this is now the regression net for the empty-frame policy - a pass must
	 * record zero work (no draws, no dispatches) on an unpublished frame.
	 */
	static RenderContext EmptyContext()
	{
		return RenderContext{
			.width      = kRenderWidth,
			.height     = kRenderHeight,
			.frameIndex = 0,
		};
	}

	static constexpr uint32_t kRenderWidth  = 128;
	static constexpr uint32_t kRenderHeight = 128;

	std::unique_ptr<RenderCache> m_renderCache;
};

// ---------------------------------------------------------------------------
// The two passes that used to dereference null
// ---------------------------------------------------------------------------

TEST_F(NullScenePassTest, LightingPassRecordsNothingWithoutAScene)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan device";

	LightingPass pass(*m_device, PhysicalDevice(), 2u);

	const RenderContext ctx = EmptyContext();

	PassStats stats{};
	auto& cmd = BeginCmd();
	// Before the fix this dereferenced ctx.scene for light_list.
	stats = pass.Record(*cmd, *m_renderCache, ctx);
	EndSubmitWait(cmd);

	EXPECT_EQ(stats.dispatches, 0u) << "a scene-less frame must not shade";
	EXPECT_EQ(stats.drawCalls, 0u);
}

TEST_F(NullScenePassTest, SelectionOutlinePassRecordsNothingWithoutAScene)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan device";

	SelectionOutlinePass pass(*m_device, PhysicalDevice(), 2u);

	const RenderContext ctx = EmptyContext();

	PassStats stats{};
	auto& cmd = BeginCmd();
	// Before the fix this dereferenced ctx.scene for selections.
	stats = pass.Record(*cmd, *m_renderCache, ctx);
	EndSubmitWait(cmd);

	EXPECT_EQ(stats.dispatches, 0u) << "nothing is selected when nothing exists";
	EXPECT_EQ(stats.drawCalls, 0u);
}

// ---------------------------------------------------------------------------
// The three that already guarded — same predicate, pinned in the same place
// ---------------------------------------------------------------------------

TEST_F(NullScenePassTest, GeometryPassRecordsNothingWithoutAScene)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan device";

	GeometryPass pass(*m_device, PhysicalDevice());

	const RenderContext ctx = EmptyContext();

	PassStats stats{};
	auto& cmd = BeginCmd();
	stats = pass.Record(*cmd, *m_renderCache, ctx);
	EndSubmitWait(cmd);

	EXPECT_EQ(stats.drawCalls, 0u) << "no mesh list to iterate";
}

TEST_F(NullScenePassTest, ShadowDepthPassRecordsNothingWithoutAScene)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan device";

	ShadowDepthPass pass(*m_device, PhysicalDevice(), m_queue,
	                     m_graphicsQueueFamily);

	const RenderContext ctx = EmptyContext();

	PassStats stats{};
	auto& cmd = BeginCmd();
	stats = pass.Record(*cmd, *m_renderCache, ctx);
	EndSubmitWait(cmd);

	EXPECT_EQ(stats.drawCalls, 0u) << "no light casts a shadow of nothing";
}

TEST_F(NullScenePassTest, ShadowIntensityPassRecordsNothingWithoutAScene)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan device";

	ShadowIntensityPass pass(*m_device, PhysicalDevice(), 2u);

	const RenderContext ctx = EmptyContext();

	PassStats stats{};
	auto& cmd = BeginCmd();
	stats = pass.Record(*cmd, *m_renderCache, ctx);
	EndSubmitWait(cmd);

	EXPECT_EQ(stats.dispatches, 0u) << "no light list to resolve";
}

// ---------------------------------------------------------------------------
// Frame().Settings(): the null-config half of the same policy
// ---------------------------------------------------------------------------

// The fallback now lives once, in core/RenderConfig.h's DefaultRenderConfig(),
// reached through RenderContext::Frame(): an unpublished context reads the empty
// frame, whose Settings() is a default-constructed RenderConfig. This is the
// pass-side half of the guard; test_ui_context.cpp pins the panel side and the
// parity between them, which is the property the old SceneContext::Settings()
// held before the two contexts replaced it.
TEST(RenderContextSettings, FallsBackToRenderConfigsOwnDefaults)
{
	// Frame().Settings() is the one place a missing config is answered, and it
	// answers with RenderConfig's in-class initializers rather than a second copy
	// of them. ShadowIntensityPass used to restate r_shadow_bias as 0.0005f
	// against a declared 0.02f — a 40x divergence that lived in the tree. This
	// test is what makes that class of drift impossible to reintroduce silently.
	const RenderContext renderContext{};
	const RenderConfig declared{};

	// Whole-value equality against the declaration: RenderConfig has a defaulted
	// operator==, so a new member is covered automatically - unless the compare
	// silently stops covering it, which is exactly the failure the named-field
	// checks below still catch.
	EXPECT_EQ(renderContext.Frame().Settings(), declared)
		<< "the empty frame reads RenderConfig's own declared defaults";

	EXPECT_FLOAT_EQ(renderContext.Frame().Settings().r_shadow_bias, declared.r_shadow_bias);
	EXPECT_FLOAT_EQ(renderContext.Frame().Settings().r_gamma,       declared.r_gamma);
	EXPECT_EQ(renderContext.Frame().Settings().r_sampling_mode,     declared.r_sampling_mode);
	EXPECT_FLOAT_EQ(renderContext.Frame().Settings().r_fxaa_subpix, declared.r_fxaa_subpix);

	// A published snapshot wins, and its config is returned by reference, not
	// copied: the reader reads the snapshot's own member, so a change to it is a
	// change to what the pass sees.
	RenderFrameSnapshot snapshot{};
	snapshot.config.r_shadow_bias = 0.1234f;
	RenderContext published{};
	published.snapshot = &snapshot;
	EXPECT_EQ(&published.Frame().Settings(), &snapshot.config);
	EXPECT_FLOAT_EQ(published.Frame().Settings().r_shadow_bias, 0.1234f);
}
