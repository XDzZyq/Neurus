/**
 * @file test_render_config_controller.cpp
 * @brief RenderConfigController undo/redo tests (no GPU).
 *
 * RenderConfig is scene-level (not per-object) state, mutated on the single
 * controller path. These tests exercise that path end to end: a
 * RenderConfigChangedEvent applies the config into the Editor-owned RenderConfig
 * (reached through a provider) and records a SetRenderConfigOp, then Undo/Redo
 * replay the stored before/after config synchronously.
 *
 * Two recording strategies are covered:
 *   - Discrete edit (checkbox/combo): no gesture, recorded immediately, one op.
 *   - Slider drag: bounded by ConfigEditBegin/ConfigEditEnd — the whole stream
 *     of intermediate values collapses to a single op committed on release.
 *
 * Replay dispatches the stored config event on the bus only (no scene or
 * resource access), so the fixture needs no object pool.
 */

#include <gtest/gtest.h>

#include <memory>

#include "editor/controllers/RenderConfigController.h"
#include "editor/events/ConfigEvents.h"
#include "editor/events/EventBus.h"
#include "editor/operations/ConfigOperations.h"
#include "editor/operations/OperationContext.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "scene/Scene.h"

using namespace neurus;

namespace {

/** @brief Builds a RenderConfig with a distinct gamma (used as a probe value). */
RenderConfig ConfigWithGamma(float gamma)
{
	RenderConfig cfg;
	cfg.r_gamma = gamma;
	return cfg;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------

class RenderConfigControllerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_controller = std::make_unique<RenderConfigController>();
		m_controller->Init(m_ctx);
	}

	void Process() { m_eventBus.Process(); }

	EventQueue m_eventBus;
	Scene m_scene; // real scene so OperationManager::Replay executes
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config; // stands in for the Editor-owned live config
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() { return &m_scene; },
	                         [this]() { return &m_config; } };
	std::unique_ptr<RenderConfigController> m_controller;
};

// --- Discrete edit ----------------------------------------------------------

TEST_F(RenderConfigControllerTest, DiscreteEdit_AppliesAndRecords_RoundTrip)
{
	const float original = m_config.r_gamma;
	const RenderConfig edited = ConfigWithGamma(2.2f);

	m_eventBus.enqueue(RenderConfigChangedEvent{ edited });
	Process();
	EXPECT_FLOAT_EQ(m_config.r_gamma, 2.2f);
	ASSERT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());

	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_config.r_gamma, original);
	EXPECT_FALSE(m_operations.CanUndo());
	ASSERT_TRUE(m_operations.CanRedo());

	m_operations.Redo();
	EXPECT_FLOAT_EQ(m_config.r_gamma, 2.2f);
	EXPECT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
}

TEST_F(RenderConfigControllerTest, NoOpEdit_RecordsNothing)
{
	// Writing the current (default) config back is a no-op: applied but not
	// recorded, so the undo history stays empty.
	m_eventBus.enqueue(RenderConfigChangedEvent{ m_config });
	Process();
	EXPECT_FALSE(m_operations.CanUndo());
}

// --- Slider gesture ---------------------------------------------------------

TEST_F(RenderConfigControllerTest, SliderGesture_CollapsesToOneUndoEntry)
{
	const float original = m_config.r_gamma;

	// Bracket a stream of intermediate slider values with begin/end. Every
	// intermediate write applies live but is NOT recorded; only the release
	// commits one op spanning the whole drag.
	m_eventBus.enqueue(ConfigEditBegin{});
	m_eventBus.enqueue(RenderConfigChangedEvent{ ConfigWithGamma(1.5f) });
	m_eventBus.enqueue(RenderConfigChangedEvent{ ConfigWithGamma(2.0f) });
	m_eventBus.enqueue(RenderConfigChangedEvent{ ConfigWithGamma(2.2f) });
	m_eventBus.enqueue(ConfigEditEnd{});
	Process();

	EXPECT_FLOAT_EQ(m_config.r_gamma, 2.2f);
	ASSERT_TRUE(m_operations.CanUndo());

	// A single undo returns to the pre-gesture config, proving one entry
	// spanning original -> 2.2 (not three separate intermediate steps).
	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_config.r_gamma, original);
	EXPECT_FALSE(m_operations.CanUndo());

	m_operations.Redo();
	EXPECT_FLOAT_EQ(m_config.r_gamma, 2.2f);
}

TEST_F(RenderConfigControllerTest, SliderGesture_NetNoChange_RecordsNothing)
{
	const float original = m_config.r_gamma;

	// Drag away and back to the starting value: at release the before/after
	// configs are equal, so nothing is recorded.
	m_eventBus.enqueue(ConfigEditBegin{});
	m_eventBus.enqueue(RenderConfigChangedEvent{ ConfigWithGamma(2.0f) });
	m_eventBus.enqueue(RenderConfigChangedEvent{ ConfigWithGamma(original) });
	m_eventBus.enqueue(ConfigEditEnd{});
	Process();

	EXPECT_FLOAT_EQ(m_config.r_gamma, original);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(RenderConfigControllerTest, DuringGesture_NoIntermediateEntries)
{
	// Mid-gesture, before release, no op has been recorded yet even though the
	// live config already reflects the latest intermediate value.
	m_eventBus.enqueue(ConfigEditBegin{});
	m_eventBus.enqueue(RenderConfigChangedEvent{ ConfigWithGamma(1.7f) });
	Process();

	EXPECT_FLOAT_EQ(m_config.r_gamma, 1.7f);
	EXPECT_FALSE(m_operations.CanUndo());

	m_eventBus.enqueue(ConfigEditEnd{});
	Process();
	EXPECT_TRUE(m_operations.CanUndo());
}

// --- Operation involution ---------------------------------------------------

TEST_F(RenderConfigControllerTest, SetRenderConfigOp_Inverse_IsInvolution)
{
	const RenderConfig before = ConfigWithGamma(1.0f);
	const RenderConfig after = ConfigWithGamma(2.2f);
	auto op = std::make_unique<SetRenderConfigOp>(before, after);
	auto twice = op->Inverse()->Inverse();

	EXPECT_EQ(twice->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	twice->Apply(ctx); // (g⁻¹)⁻¹ reproduces the original forward effect.
	EXPECT_FLOAT_EQ(m_config.r_gamma, 2.2f);
}

TEST_F(RenderConfigControllerTest, SetRenderConfigOp_Inverse_AppliesBefore)
{
	const RenderConfig before = ConfigWithGamma(1.0f);
	const RenderConfig after = ConfigWithGamma(2.2f);
	auto op = std::make_unique<SetRenderConfigOp>(before, after);
	auto inv = op->Inverse();

	OperationContext ctx{ m_eventBus };
	inv->Apply(ctx); // inverse applies the "before" config.
	EXPECT_FLOAT_EQ(m_config.r_gamma, 1.0f);
}
