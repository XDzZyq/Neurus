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
#include <sstream>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "asset/Project.hpp"
#include <cereal/archives/json.hpp>
#include "asset/components/ConfigComponent.hpp"

#include "editor/controllers/RenderConfigController.hpp"
#include "editor/events/ConfigEvents.hpp"
#include "editor/events/EventBus.hpp"
#include "editor/operations/ConfigOperations.hpp"
#include "shared/OperationTestAccess.hpp"
#include "editor/operations/OperationManager.hpp"
#include "core/ResourceManager.hpp"
#include "core/RenderConfig.hpp"
#include "scene/Scene.hpp"

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

	m_eventBus.Enqueue(RenderConfigChangedEvent{ edited });
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
	m_eventBus.Enqueue(RenderConfigChangedEvent{ m_config });
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
	m_eventBus.Enqueue(ConfigEditBegin{});
	m_eventBus.Enqueue(RenderConfigChangedEvent{ ConfigWithGamma(1.5f) });
	m_eventBus.Enqueue(RenderConfigChangedEvent{ ConfigWithGamma(2.0f) });
	m_eventBus.Enqueue(RenderConfigChangedEvent{ ConfigWithGamma(2.2f) });
	m_eventBus.Enqueue(ConfigEditEnd{});
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
	m_eventBus.Enqueue(ConfigEditBegin{});
	m_eventBus.Enqueue(RenderConfigChangedEvent{ ConfigWithGamma(2.0f) });
	m_eventBus.Enqueue(RenderConfigChangedEvent{ ConfigWithGamma(original) });
	m_eventBus.Enqueue(ConfigEditEnd{});
	Process();

	EXPECT_FLOAT_EQ(m_config.r_gamma, original);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(RenderConfigControllerTest, DuringGesture_NoIntermediateEntries)
{
	// Mid-gesture, before release, no op has been recorded yet even though the
	// live config already reflects the latest intermediate value.
	m_eventBus.Enqueue(ConfigEditBegin{});
	m_eventBus.Enqueue(RenderConfigChangedEvent{ ConfigWithGamma(1.7f) });
	Process();

	EXPECT_FLOAT_EQ(m_config.r_gamma, 1.7f);
	EXPECT_FALSE(m_operations.CanUndo());

	m_eventBus.Enqueue(ConfigEditEnd{});
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

	auto ctx = OperationTestAccess::Context(m_eventBus);
	twice->Apply(ctx); // (g⁻¹)⁻¹ reproduces the original forward effect.
	EXPECT_FLOAT_EQ(m_config.r_gamma, 2.2f);
}

TEST_F(RenderConfigControllerTest, SetRenderConfigOp_Inverse_AppliesBefore)
{
	const RenderConfig before = ConfigWithGamma(1.0f);
	const RenderConfig after = ConfigWithGamma(2.2f);
	auto op = std::make_unique<SetRenderConfigOp>(before, after);
	auto inv = op->Inverse();

	auto ctx = OperationTestAccess::Context(m_eventBus);
	inv->Apply(ctx); // inverse applies the "before" config.
	EXPECT_FLOAT_EQ(m_config.r_gamma, 1.0f);
}

class OptionalRenderConfigControllerTest : public ::testing::Test
{
protected:
	void SetUp() override { m_controller.Init(m_context); }
	EventQueue m_events;
	OperationManager m_operations{m_events};
	ResourceManager m_resources;
	std::shared_ptr<RenderConfig> m_data;
	ControllerContext m_context{m_events, m_resources, m_operations, {},
		[this]() { return m_data.get(); }, {},
		[this]() -> std::shared_ptr<RenderConfig>& { return m_data; }};
	RenderConfigController m_controller;
};

TEST_F(OptionalRenderConfigControllerTest, GestureWithoutEditKeepsConfigAbsent)
{
	m_events.Emit(ConfigEditBegin{});
	m_events.Emit(ConfigEditEnd{});
	EXPECT_FALSE(m_data);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(OptionalRenderConfigControllerTest, ExplicitDefaultEditIsUndoableToAbsence)
{
	m_events.Emit(RenderConfigChangedEvent{RenderConfig{}});
	ASSERT_NE(m_data, nullptr);
	EXPECT_EQ(*m_data, RenderConfig{});
	ASSERT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FALSE(m_data);
	m_operations.Redo();
	ASSERT_NE(m_data, nullptr);
	EXPECT_EQ(*m_data, RenderConfig{});
}

TEST_F(OptionalRenderConfigControllerTest, GestureUndoRestoresAbsence)
{
	m_events.Emit(ConfigEditBegin{});
	m_events.Emit(RenderConfigChangedEvent{ConfigWithGamma(1.5f)});
	m_events.Emit(RenderConfigChangedEvent{ConfigWithGamma(2.2f)});
	m_events.Emit(ConfigEditEnd{});
	ASSERT_NE(m_data, nullptr);
	EXPECT_FLOAT_EQ(m_data->r_gamma, 2.2f);
	m_operations.Undo();
	EXPECT_FALSE(m_data);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(OptionalRenderConfigControllerTest, SerializedOperationRetainsAbsentEndpoint)
{
	SetRenderConfigOp source(std::nullopt, ConfigWithGamma(2.2f));
	std::stringstream serialized;
	{ cereal::JSONOutputArchive archive(serialized); archive(source); }
	EXPECT_EQ(serialized.str().find("\"before\""), std::string::npos);
	SetRenderConfigOp restored;
	{ cereal::JSONInputArchive archive(serialized); archive(restored); }
	auto context = OperationTestAccess::Context(m_events);
	restored.Apply(context);
	ASSERT_NE(m_data, nullptr);
	EXPECT_FLOAT_EQ(m_data->r_gamma, 2.2f);
	restored.Inverse()->Apply(context);
	EXPECT_FALSE(m_data);
}

namespace {
std::shared_ptr<RenderConfig> LoadOptionalConfig(const std::string& json)
{
	auto data = std::make_shared<RenderConfig>();
	data->r_gamma = 9.0f;
	std::stringstream input(json);
	cereal::JSONInputArchive archive(input);
	project::ConfigComponent component(data);
	component.Load(archive);
	return data;
}

std::string SaveOptionalConfig(std::shared_ptr<RenderConfig>& data)
{
	std::stringstream output;
	{ cereal::JSONOutputArchive archive(output); project::ConfigComponent component(data); component.Save(archive); }
	return output.str();
}
}

TEST(OptionalConfigPersistence, MissingConfigStaysAbsentOnSave)
{
	auto data = LoadOptionalConfig("{}");
	EXPECT_FALSE(data);
	EXPECT_EQ(SaveOptionalConfig(data).find("proj_config"), std::string::npos);
}

TEST(OptionalConfigPersistence, MalformedConfigStaysAbsentOnSave)
{
	auto data = LoadOptionalConfig(R"({"proj_config":42})");
	EXPECT_FALSE(data);
	EXPECT_EQ(SaveOptionalConfig(data).find("proj_config"), std::string::npos);
}

TEST(OptionalConfigPersistence, PartialConfigKeepsFieldDefaultsAndIsSaved)
{
	auto data = LoadOptionalConfig(R"({"proj_config":{"r_gamma":2.5}})");
	ASSERT_NE(data, nullptr);
	EXPECT_FLOAT_EQ(data->r_gamma, 2.5f);
	EXPECT_FLOAT_EQ(data->r_shadow_bias, RenderConfig{}.r_shadow_bias);
	const auto saved = SaveOptionalConfig(data);
	EXPECT_NE(saved.find("proj_config"), std::string::npos);
	auto restored = LoadOptionalConfig(saved);
	ASSERT_NE(restored, nullptr);
	EXPECT_EQ(*restored, *data);
}

TEST(OptionalConfigPersistence, ExplicitNullConfigStaysAbsentOnSave)
{
	auto data = LoadOptionalConfig(R"({"proj_config":null})");
	EXPECT_FALSE(data);
	EXPECT_EQ(SaveOptionalConfig(data).find("proj_config"), std::string::npos);
}

TEST_F(OptionalRenderConfigControllerTest, LegacyOperationWithoutPresenceFlagsLoads)
{
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		auto before = ConfigWithGamma(1.5f);
		auto after = ConfigWithGamma(2.2f);
		archive(cereal::make_nvp("before", before), cereal::make_nvp("after", after));
	}
	SetRenderConfigOp restored;
	{ cereal::JSONInputArchive archive(serialized); restored.serialize(archive); }
	auto context = OperationTestAccess::Context(m_events);
	restored.Apply(context);
	ASSERT_NE(m_data, nullptr);
	EXPECT_FLOAT_EQ(m_data->r_gamma, 2.2f);
	restored.Inverse()->Apply(context);
	ASSERT_NE(m_data, nullptr);
	EXPECT_FLOAT_EQ(m_data->r_gamma, 1.5f);
}

TEST(OptionalConfigPersistence, ProjectFileRecoversMalformedFieldsIndependently)
{
	const std::string unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	struct TemporaryProjects
	{
		std::filesystem::path input;
		std::filesystem::path saved;
		~TemporaryProjects()
		{
			std::error_code error;
			std::filesystem::remove(input, error);
			std::filesystem::remove(saved, error);
		}
	} files{std::filesystem::temp_directory_path() / ("neurus-config-input-" + unique + ".json"),
	        std::filesystem::temp_directory_path() / ("neurus-config-saved-" + unique + ".json")};
	{
		std::ofstream output(files.input);
		output << R"({"project":{"proj_config":{"r_gamma":"bad","r_shadow_bias":0.004,
		  "r_sampling_count":"bad","r_ao_ksize":4.5,"r_sample_pf":999999999999,
		  "r_sampling_mode":-1,"r_transparent":1,"r_aa":99,"r_pipeline":-1,
		  "r_ao":4,"r_shadow":-1,"r_ssr":99,"r_ao_radius":1e100,
		  "r_debug_draw":false}}})";
	}
	std::shared_ptr<RenderConfig> data;
	project::Project document;
	document.Register<project::ConfigComponent>(data);
	ASSERT_NO_THROW(document.Load(files.input.string()));
	ASSERT_NE(data, nullptr);
	RenderConfig expected;
	expected.r_shadow_bias = 0.004f;
	expected.r_debug_draw = false;
	EXPECT_EQ(*data, expected);
	ASSERT_NO_THROW(document.Save(files.saved.string()));
	data.reset();
	ASSERT_NO_THROW(document.Load(files.saved.string()));
	ASSERT_NE(data, nullptr);
	EXPECT_EQ(*data, expected);
}
