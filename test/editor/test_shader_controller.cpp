/**
 * @file test_shader_controller.cpp
 * @brief ShaderController undo/redo tests for content edits (no GPU).
 *
 * Shader content edits are per-object state (a mesh's shader stage). Each edit
 * dimension is recorded as its own delta op, keyed on the mesh UID:
 *   - Code text   -> SetShaderCodeOp   (before/after GLSL)
 *   - Struct field -> SetShaderFieldOp  (before/after of one IR element)
 *   - Field add    -> AddShaderFieldOp  (append/remove one default entry)
 *
 * These tests exercise that path end to end: events applied by the controller
 * mutate the live ShaderUnit and record an op; Undo/Redo replay the stored
 * delta synchronously (via ShaderCodeRestored / ShaderFieldRestored /
 * ShaderFieldAddRestored / ShaderFieldRemoved), re-applying the edit dimension
 * and bumping the ShaderUnit version so the panel refreshes.
 *
 * Two recording strategies are covered:
 *   - Code edit: bounded by ShaderEditBegin/ShaderEditEnd — the keystroke burst
 *     (ShaderCodeEdited) applies live but is NOT recorded; one op is committed
 *     on focus-out if the code actually changed.
 *   - Discrete edit (struct-field / field-add): no gesture, recorded immediately.
 *
 * The fixture supplies a real scene holding the mesh so the controller can
 * resolve the event UIDs back to the live object at dispatch/replay time.
 *
 * Create Shader IS undoable via ShaderLinkOp (pool-preserving membership
 * toggle) and is exercised in ShaderCreateUndoTest below; Compile stays a
 * non-undoable lifecycle action (GPU compilation not exercised here).
 */

#include <gtest/gtest.h>

#include <memory>
#include <sstream>

#include <cereal/archives/json.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/polymorphic.hpp>

#include "editor/controllers/ShaderController.h"
#include "editor/events/EventBus.h"
#include "editor/events/ShaderEvents.h"
#include "editor/operations/OperationContext.h"
#include "editor/operations/OperationManager.h"
#include "editor/operations/registrations/OperationRegistration.h"
#include "editor/operations/ShaderOperations.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "render/shaders/RenderShader.h"
#include "render/shaders/Shader.h"
#include "render/shaders/ShaderParser.h"
#include "render/shaders/ShaderUnit.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "core/ResourceManager.h"

#include <fstream>
#include <sstream>

using namespace neurus;

namespace {

/** @brief Serializes an op through a base pointer and reconstructs it via cereal. */
std::unique_ptr<Operation> RoundTrip(const std::unique_ptr<Operation>& op)
{
	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("op", op));
	}
	std::unique_ptr<Operation> restored;
	cereal::JSONInputArchive in(ss);
	in(cereal::make_nvp("op", restored));
	return restored;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------

class ShaderControllerTest : public ::testing::Test
{
protected:
	static constexpr int kVertex = static_cast<int>(ShaderType::VERTEX);

	void SetUp() override
	{
		m_controller.Init(m_ctx);

		// Build a mesh with a CPU-only shader carrying seeded vertex+fragment
		// stages. GetStage() default-inserts the stage, so HasStage() is true.
		m_mesh = std::make_shared<Mesh>();
		auto shader = std::make_shared<RenderShader>("TestShader", "v.vert", "f.frag");
		shader->GetStage(ShaderType::VERTEX).code = "VERT_V0";
		shader->GetStage(ShaderType::FRAGMENT).code = "FRAG_V0";
		m_mesh->o_shader = shader;
		m_scene.UseMesh(m_mesh);
	}

	void Process() { m_eventBus.Process(); }

	/** @brief The live vertex ShaderUnit for assertions. */
	ShaderUnit& VertexUnit() { return m_mesh->o_shader->GetStage(ShaderType::VERTEX); }

	/** @brief The mesh's UID, used as the event payload. */
	int MeshObj() const { return m_mesh->GetObjectID(); }

	EventQueue m_eventBus;
	Scene m_scene; // real scene so event UIDs resolve at dispatch/replay
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config;
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() { return &m_scene; },
	                         [this]() { return &m_config; } };
	ShaderController m_controller;
	std::shared_ptr<Mesh> m_mesh;
};

// --- Code-edit gesture ------------------------------------------------------

TEST_F(ShaderControllerTest, CodeEditGesture_AppliesAndRecords_RoundTrip)
{
	// Begin gesture -> live keystroke -> end gesture: one op on focus-out.
	m_eventBus.enqueue(ShaderEditBegin{ MeshObj(), kVertex });
	m_eventBus.enqueue(ShaderCodeEdited{ MeshObj(), kVertex, "VERT_V1" });
	m_eventBus.enqueue(ShaderEditEnd{ MeshObj(), kVertex });
	Process();

	EXPECT_EQ(VertexUnit().code, "VERT_V1");
	ASSERT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());

	const int versionAfterEdit = VertexUnit().GetVersion();

	m_operations.Undo();
	EXPECT_EQ(VertexUnit().code, "VERT_V0");
	EXPECT_GT(VertexUnit().GetVersion(), versionAfterEdit); // restore bumps version
	EXPECT_FALSE(m_operations.CanUndo());
	ASSERT_TRUE(m_operations.CanRedo());

	m_operations.Redo();
	EXPECT_EQ(VertexUnit().code, "VERT_V1");
	EXPECT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
}

TEST_F(ShaderControllerTest, CodeEditGesture_NoNetChange_RecordsNothing)
{
	// Begin/end with no intervening code change: nothing recorded.
	m_eventBus.enqueue(ShaderEditBegin{ MeshObj(), kVertex });
	m_eventBus.enqueue(ShaderEditEnd{ MeshObj(), kVertex });
	Process();

	EXPECT_EQ(VertexUnit().code, "VERT_V0");
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(ShaderControllerTest, CodeEditGesture_DrainAwayAndBack_RecordsNothing)
{
	// Edit within a gesture then return to the original code: net no-op.
	m_eventBus.enqueue(ShaderEditBegin{ MeshObj(), kVertex });
	m_eventBus.enqueue(ShaderCodeEdited{ MeshObj(), kVertex, "VERT_TEMP" });
	m_eventBus.enqueue(ShaderCodeEdited{ MeshObj(), kVertex, "VERT_V0" });
	m_eventBus.enqueue(ShaderEditEnd{ MeshObj(), kVertex });
	Process();

	EXPECT_EQ(VertexUnit().code, "VERT_V0");
	EXPECT_FALSE(m_operations.CanUndo());
}

// --- Discrete field-add edit ------------------------------------------------

TEST_F(ShaderControllerTest, FieldAdd_AppliesAndRecords_RoundTrip)
{
	ASSERT_TRUE(VertexUnit().parsed.AB_list.empty());

	// Adding an attribute is a discrete edit: recorded immediately, one op.
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::Attributes, -1 });
	Process();

	ASSERT_EQ(VertexUnit().parsed.AB_list.size(), 1u);
	ASSERT_TRUE(m_operations.CanUndo());

	m_operations.Undo();
	EXPECT_TRUE(VertexUnit().parsed.AB_list.empty()); // parsed IR restored
	ASSERT_TRUE(m_operations.CanRedo());

	m_operations.Redo();
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), 1u);
}

// --- Discrete struct-field edit ---------------------------------------------

TEST_F(ShaderControllerTest, StructEdit_RenamesField_RoundTrip)
{
	// Seed one attribute so there is a field to rename.
	VertexUnit().parsed.AB_list.push_back(
		S_IO{ 0, "orig", ParaType::FLOAT, "", Interp::Smooth });

	m_eventBus.enqueue(ShaderStructEdited{
		MeshObj(), kVertex, ShaderSection::Attributes, /*fieldIndex*/ 0,
		/*subFieldIndex*/ -1, /*field*/ "name", /*value*/ "renamed" });
	Process();

	ASSERT_EQ(VertexUnit().parsed.AB_list.size(), 1u);
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "renamed");
	ASSERT_TRUE(m_operations.CanUndo());

	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "orig");

	m_operations.Redo();
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "renamed");
}

TEST_F(ShaderControllerTest, StructEdit_NoMatchingField_RecordsNothing)
{
	// fieldIndex out of range: VisitElement returns false, no op recorded.
	m_eventBus.enqueue(ShaderStructEdited{
		MeshObj(), kVertex, ShaderSection::Attributes, /*fieldIndex*/ 5,
		/*subFieldIndex*/ -1, /*field*/ "name", /*value*/ "x" });
	Process();

	EXPECT_FALSE(m_operations.CanUndo());
}

// --- SetShaderCodeOp: involution + serialization ----------------------------

TEST_F(ShaderControllerTest, SetShaderCodeOp_Inverse_IsInvolution)
{
	const int uid = m_mesh->GetObjectID();
	auto op = std::make_unique<SetShaderCodeOp>(uid, kVertex, "CODE_A", "CODE_B");
	auto twice = op->Inverse()->Inverse();

	EXPECT_EQ(twice->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	twice->Apply(ctx); // (g⁻¹)⁻¹ reproduces the original forward effect.
	EXPECT_EQ(VertexUnit().code, "CODE_B");
}

TEST_F(ShaderControllerTest, SetShaderCodeOp_Inverse_AppliesBefore)
{
	const int uid = m_mesh->GetObjectID();
	auto op = std::make_unique<SetShaderCodeOp>(uid, kVertex, "CODE_A", "CODE_B");
	auto inv = op->Inverse();

	OperationContext ctx{ m_eventBus };
	inv->Apply(ctx); // inverse applies the "before" code.
	EXPECT_EQ(VertexUnit().code, "CODE_A");
}

TEST_F(ShaderControllerTest, SetShaderCodeOp_Serialize_RoundTrip)
{
	const int uid = m_mesh->GetObjectID();
	std::unique_ptr<Operation> op =
		std::make_unique<SetShaderCodeOp>(uid, kVertex, "CODE_BEFORE", "CODE_AFTER");

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<SetShaderCodeOp*>(restored.get()), nullptr);
	EXPECT_EQ(restored->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	restored->Apply(ctx); // "after" survived
	EXPECT_EQ(VertexUnit().code, "CODE_AFTER");

	restored->Inverse()->Apply(ctx); // "before" + uid + stage survived
	EXPECT_EQ(VertexUnit().code, "CODE_BEFORE");
}

// --- SetShaderFieldOp: involution + serialization ---------------------------

TEST_F(ShaderControllerTest, SetShaderFieldOp_Inverse_IsInvolution)
{
	// Seed one attribute so the field op has a target to overwrite.
	VertexUnit().parsed.AB_list.push_back(
		S_IO{ 0, "orig", ParaType::FLOAT, "", Interp::Smooth });

	const int uid = m_mesh->GetObjectID();
	ShaderFieldValue before = S_IO{ 0, "orig", ParaType::FLOAT, "", Interp::Smooth };
	ShaderFieldValue after  = S_IO{ 0, "renamed", ParaType::FLOAT, "", Interp::Smooth };
	auto op = std::make_unique<SetShaderFieldOp>(
		uid, kVertex, ShaderSection::Attributes, /*fieldIndex*/ 0, before, after);
	auto twice = op->Inverse()->Inverse();

	EXPECT_EQ(twice->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	twice->Apply(ctx); // reproduces the original forward effect.
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "renamed");
}

TEST_F(ShaderControllerTest, SetShaderFieldOp_Serialize_RoundTrip)
{
	VertexUnit().parsed.AB_list.push_back(
		S_IO{ 0, "orig", ParaType::FLOAT, "", Interp::Smooth });

	const int uid = m_mesh->GetObjectID();
	ShaderFieldValue before = S_IO{ 0, "orig", ParaType::FLOAT, "", Interp::Smooth };
	ShaderFieldValue after  = S_IO{ 0, "renamed", ParaType::FLOAT, "", Interp::Smooth };
	std::unique_ptr<Operation> op = std::make_unique<SetShaderFieldOp>(
		uid, kVertex, ShaderSection::Attributes, /*fieldIndex*/ 0, before, after);

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<SetShaderFieldOp*>(restored.get()), nullptr);
	EXPECT_EQ(restored->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	restored->Apply(ctx); // "after" element + section + index survived
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "renamed");

	restored->Inverse()->Apply(ctx); // "before" element survived
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "orig");
}

// --- AddShaderFieldOp: involution + serialization ---------------------------

TEST_F(ShaderControllerTest, AddShaderFieldOp_Inverse_RemovesThenReadds)
{
	ASSERT_TRUE(VertexUnit().parsed.AB_list.empty());

	const int uid = m_mesh->GetObjectID();
	auto op = std::make_unique<AddShaderFieldOp>(
		uid, kVertex, ShaderSection::Attributes, /*subFieldIndex*/ -1, /*add*/ true);

	OperationContext ctx{ m_eventBus };
	op->Apply(ctx); // re-append default
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), 1u);

	op->Inverse()->Apply(ctx); // remove last
	EXPECT_TRUE(VertexUnit().parsed.AB_list.empty());

	// Double inverse reproduces the add.
	op->Inverse()->Inverse()->Apply(ctx);
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), 1u);
	EXPECT_EQ(op->Label(), "Add Shader Field");
}

TEST_F(ShaderControllerTest, AddShaderFieldOp_Serialize_RoundTrip)
{
	ASSERT_TRUE(VertexUnit().parsed.AB_list.empty());

	const int uid = m_mesh->GetObjectID();
	std::unique_ptr<Operation> op = std::make_unique<AddShaderFieldOp>(
		uid, kVertex, ShaderSection::Attributes, /*subFieldIndex*/ -1, /*add*/ true);

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<AddShaderFieldOp*>(restored.get()), nullptr);
	EXPECT_EQ(restored->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	restored->Apply(ctx); // add flag + section survived -> appends
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), 1u);

	restored->Inverse()->Apply(ctx); // inverse removes
	EXPECT_TRUE(VertexUnit().parsed.AB_list.empty());
}

// --- Multi-op undo over populated IR (real-app shapes) ----------------------

/**
 * @brief Seeds a realistic vertex IR: one attribute, one uniform, and two
 * struct definitions (each with two members) — the shape a real parse produces.
 */
void SeedRealisticIR(neurus::ShaderStruct& parsed)
{
	parsed.AB_list.push_back(
		S_IO{ 0, "aPosition", ParaType::VEC3, "vec3", Interp::Smooth });

	parsed.uniform_list.push_back(
		S_Uniform{ "uTime", ParaType::FLOAT, 1, 0, "", "float", "" });

	parsed.struct_def_list.push_back(
		S_StructDef{ 0, "LightData", {
			S_IO{ 0, "position", ParaType::VEC3, "vec3", Interp::Smooth },
			S_IO{ 1, "color",    ParaType::VEC3, "vec3", Interp::Smooth },
		}, "lights" });

	parsed.struct_def_list.push_back(
		S_StructDef{ 1, "Material", {
			S_IO{ 0, "albedo", ParaType::VEC3, "vec3", Interp::Smooth },
			S_IO{ 1, "roughness", ParaType::FLOAT, "float", Interp::Smooth },
		}, "mat" });
}

TEST_F(ShaderControllerTest, FieldAdd_TwoDifferentSections_UndoRestoresBoth)
{
	SeedRealisticIR(VertexUnit().parsed);
	const size_t attrs = VertexUnit().parsed.AB_list.size();
	const size_t unifs = VertexUnit().parsed.uniform_list.size();

	// Add one field to Attributes and one to Uniforms (two different sections).
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::Attributes, -1 });
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::Uniforms, -1 });
	Process();

	ASSERT_EQ(VertexUnit().parsed.AB_list.size(), attrs + 1);
	ASSERT_EQ(VertexUnit().parsed.uniform_list.size(), unifs + 1);
	ASSERT_TRUE(m_operations.CanUndo());

	// Undo the uniform add, then the attribute add.
	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.uniform_list.size(), unifs);
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), attrs + 1);

	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), attrs);

	// Undo stack is exhausted and the IR matches the original seed exactly.
	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].name, "aPosition");
	EXPECT_EQ(VertexUnit().parsed.uniform_list[0].name, "uTime");
}

TEST_F(ShaderControllerTest, FieldAdd_TwoDifferentStructDefs_UndoRestoresBoth)
{
	SeedRealisticIR(VertexUnit().parsed);
	auto& sd = VertexUnit().parsed.struct_def_list;
	const size_t a = sd[0].fields.size();
	const size_t b = sd[1].fields.size();

	// Add a member field to struct 0, then a member to struct 1.
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::StructDefs, 0 });
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::StructDefs, 1 });
	Process();

	ASSERT_EQ(sd[0].fields.size(), a + 1);
	ASSERT_EQ(sd[1].fields.size(), b + 1);

	m_operations.Undo();
	EXPECT_EQ(sd[1].fields.size(), b);
	EXPECT_EQ(sd[0].fields.size(), a + 1);

	m_operations.Undo();
	EXPECT_EQ(sd[0].fields.size(), a);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(ShaderControllerTest, FieldAdd_MixedAdds_ThenCodeEdit_UndoKeepsWorking)
{
	SeedRealisticIR(VertexUnit().parsed);
	const size_t attrs = VertexUnit().parsed.AB_list.size();

	// Two adds in different sections, then a code-edit gesture on top.
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::Attributes, -1 });
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::Uniforms, -1 });
	Process();

	m_eventBus.enqueue(ShaderEditBegin{ MeshObj(), kVertex });
	m_eventBus.enqueue(ShaderCodeEdited{ MeshObj(), kVertex, "VERT_V1" });
	m_eventBus.enqueue(ShaderEditEnd{ MeshObj(), kVertex });
	Process();
	ASSERT_EQ(VertexUnit().code, "VERT_V1");

	// Undo the code edit first (LIFO) — everything stays consistent.
	m_operations.Undo();
	EXPECT_EQ(VertexUnit().code, "VERT_V0");
	ASSERT_TRUE(m_operations.CanUndo());

	// Then undo both adds.
	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.uniform_list.size(), 1u);
	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), attrs);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(ShaderControllerTest, StructEdit_TypeEditOnRealisticIR_UndoRestores)
{
	SeedRealisticIR(VertexUnit().parsed);

	// Change the attribute's type from vec3 to vec4 (as the type combo delivers).
	m_eventBus.enqueue(ShaderStructEdited{
		MeshObj(), kVertex, ShaderSection::Attributes, /*fieldIndex*/ 0,
		/*subFieldIndex*/ -1, /*field*/ "type", /*value*/ "vec4" });
	Process();

	ASSERT_EQ(VertexUnit().parsed.AB_list[0].typeName, "vec4");
	ASSERT_TRUE(m_operations.CanUndo());

	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].typeName, "vec3");
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].type, ParaType::VEC3);

	m_operations.Redo();
	EXPECT_EQ(VertexUnit().parsed.AB_list[0].typeName, "vec4");
}

TEST_F(ShaderControllerTest, StructEdit_StructMemberEdit_UndoRestores)
{
	SeedRealisticIR(VertexUnit().parsed);

	// Rename struct 0's member 1 (fieldIndex=struct, subFieldIndex=member).
	m_eventBus.enqueue(ShaderStructEdited{
		MeshObj(), kVertex, ShaderSection::StructDefs, /*fieldIndex*/ 0,
		/*subFieldIndex*/ 1, /*field*/ "name", /*value*/ "diffuse" });
	Process();

	ASSERT_EQ(VertexUnit().parsed.struct_def_list[0].fields[1].name, "diffuse");
	ASSERT_TRUE(m_operations.CanUndo());

	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.struct_def_list[0].fields[1].name, "color");

	m_operations.Redo();
	EXPECT_EQ(VertexUnit().parsed.struct_def_list[0].fields[1].name, "diffuse");
}

// --- Real parsed IR (the app's actual shader shape) -------------------------

/**
 * @brief Parses the real gbuffer.vert from the res tree — the exact IR shape
 * the running app edits — then replays the user's reported scenario.
 */
TEST_F(ShaderControllerTest, FieldAdd_RealGbufferVert_AttributesAndPassOutputs_UndoRestores)
{
	// Load + parse the actual vertex shader the app ships with.
	const std::string path = std::string(TEST_SOURCE_DIR) + "/res/shaders/render/gbuffer.vert";
	std::ifstream file(path);
	ASSERT_TRUE(file.is_open()) << "cannot open " << path;
	std::stringstream ss;
	ss << file.rdbuf();

	auto parsed = ShaderParser::ParseShaderCode(ss.str(), ShaderType::VERTEX);
	VertexUnit().parsed = std::move(parsed);

	// Sanity: the real IR has populated attributes + pass outputs (both S_IO).
	const size_t attrs = VertexUnit().parsed.AB_list.size();
	const size_t pass  = VertexUnit().parsed.pass_list.size();
	ASSERT_GT(attrs, 0u);
	ASSERT_GT(pass, 0u);

	// The reported combo: one add into Attributes, one into Pass Outputs.
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::Attributes, -1 });
	m_eventBus.enqueue(ShaderFieldAdded{ MeshObj(), kVertex, ShaderSection::PassOutputs, -1 });
	Process();

	ASSERT_EQ(VertexUnit().parsed.AB_list.size(), attrs + 1);
	ASSERT_EQ(VertexUnit().parsed.pass_list.size(), pass + 1);

	// Undo the pass-output add, then the attribute add.
	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.pass_list.size(), pass);
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), attrs + 1);

	m_operations.Undo();
	EXPECT_EQ(VertexUnit().parsed.AB_list.size(), attrs);
	EXPECT_FALSE(m_operations.CanUndo());

	// The surviving entries are untouched — LIFO removal hit the right lists.
	EXPECT_EQ(VertexUnit().parsed.AB_list.front().name, "inPosition");
	EXPECT_EQ(VertexUnit().parsed.pass_list.front().name, "fragWorldPos");
}

// ---------------------------------------------------------------------------
// Create Shader undo/redo (ShaderLinkOp) - pool-preserving membership toggle
// ---------------------------------------------------------------------------

class ShaderCreateUndoTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Pooled mesh registered in a real scene (so the op's stored UID
		// resolves at replay) + a pooled RenderShader the restore handlers
		// relink by UID. Subscriptions mirror Editor::Initialize's
		// ShaderLinkRestored / ShaderUnlinkRestored handlers (RenderResetEvent
		// omitted - not needed in a unit test).
		m_mesh = m_pool.Load<Mesh>();
		m_scene.UseMesh(m_mesh);
		m_shader = m_pool.Load<RenderShader>("MeshShader_1", "v.vert", "f.frag");

		m_eventBus.subscribe<ShaderLinkRestored>([this](const ShaderLinkRestored& e) {
			auto mesh = m_pool.Get<Mesh>(e.objectUid);
			if (!mesh) return;
			mesh->SetObjShader(m_pool.Get<RenderShader>(e.shaderId));
		});
		m_eventBus.subscribe<ShaderUnlinkRestored>([this](const ShaderUnlinkRestored& e) {
			auto mesh = m_pool.Get<Mesh>(e.objectUid);
			if (!mesh) return;
			mesh->SetObjShader(nullptr);
		});
	}

	EventQueue m_eventBus;
	Scene m_scene;
	ResourceManager m_pool;
	OperationManager m_operations{ m_eventBus };
	std::shared_ptr<Mesh> m_mesh;
	std::shared_ptr<RenderShader> m_shader;
};

TEST_F(ShaderCreateUndoTest, UndoDropsRedoRelinksSamePooledShader)
{
	const int shaderId = m_shader->GetObjectID();

	// Mirror Editor::OnCreateShader: link the pooled shader, then record.
	m_mesh->SetObjShader(m_shader);
	m_operations.Submit(std::make_unique<ShaderLinkOp>(m_mesh->GetObjectID(), shaderId, true));

	EXPECT_EQ(m_mesh->o_shaderId, shaderId);

	// Undo: reference dropped, pool keeps the shader.
	m_operations.Undo();
	EXPECT_EQ(m_mesh->o_shader, nullptr);
	EXPECT_EQ(m_mesh->o_shaderId, 0);
	EXPECT_NE(m_pool.Get<RenderShader>(shaderId), nullptr);

	// Redo: same pooled shader relinked (no new pooled object minted).
	m_operations.Redo();
	ASSERT_NE(m_mesh->o_shader, nullptr);
	EXPECT_EQ(m_mesh->o_shaderId, shaderId);
	EXPECT_EQ(m_mesh->o_shader->GetObjectID(), shaderId);
}

TEST_F(ShaderCreateUndoTest, StaleMeshUidNoOps)
{
	// A mesh that no longer exists resolves to null and must no-op safely.
	m_operations.Submit(std::make_unique<ShaderLinkOp>(123456, m_shader->GetObjectID(), true));
	EXPECT_NO_THROW(m_operations.Undo());
	EXPECT_NO_THROW(m_operations.Redo());
	// Stacks advance like any other op (stale-UID no-op does not alter them);
	// the unresolved mesh is never linked.
	EXPECT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
	EXPECT_EQ(m_mesh->o_shader, nullptr);
	EXPECT_EQ(m_mesh->o_shaderId, 0);
}

TEST_F(ShaderCreateUndoTest, RoundTripsThroughCereal)
{
	const int shaderId = m_shader->GetObjectID();
	std::unique_ptr<Operation> op =
		std::make_unique<ShaderLinkOp>(m_mesh->GetObjectID(), shaderId, true);

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<ShaderLinkOp*>(restored.get()), nullptr);
	EXPECT_EQ(restored->Label(), "Create Shader");

	OperationContext ctx{ m_eventBus };
	restored->Apply(ctx);
	EXPECT_EQ(m_mesh->o_shaderId, shaderId);

	restored->Inverse()->Apply(ctx);
	EXPECT_EQ(m_mesh->o_shader, nullptr);
	EXPECT_EQ(m_mesh->o_shaderId, 0);
}
