/**
 * @file test_shader_create.cpp
 * @brief Pooled shader asset loading and mesh shader creation.
 *
 * Uses a headless Editor: no renderer or UI, but the real event bus,
 * controllers and resource pool.
 */

#include <gtest/gtest.h>

#include <cereal/archives/json.hpp>

#include <sstream>
#include <string>

#include "shader/BuiltInRenderData.hpp"
#include "editor/RenderManager.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/ShaderData.hpp"
#include "editor/Editor.hpp"
#include "editor/events/AssetEvents.hpp"
#include "editor/events/OperationEvents.hpp"
#include "editor/events/SceneEvents.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

class ShaderCreateTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_editor.Initialize();

		auto mesh = m_editor.GetResourceManager().Load<neurus::Mesh>();
		mesh->o_name = m_editor.GetResourceManager().SetName(mesh->GetObjectID(), "Ground");
		m_editor.GetScene().UseMesh(mesh);
		m_groundUid = mesh->GetObjectID();
	}

	int GroundUid() const { return m_groundUid; }

	neurus::Editor m_editor{nullptr};
	int m_groundUid = 0;
};

TEST_F(ShaderCreateTest, ImportSuppliesTheMissingGraphicsStage)
{
	struct Case { const char* path; const char* name; };
	for (const Case row : { Case{"res/shaders/render/gbuffer.frag.glsl", "MyFrag"},
	                        Case{"res/shaders/render/gbuffer.vert.glsl", "MyVert"} })
	{
		SCOPED_TRACE(row.path);
		m_editor.ClearDirty();
		neurus::ShaderRequestResult result;
		m_editor.GetEventQueue().Emit(neurus::ShaderImportRequested{row.path, row.name, &result});
		ASSERT_NE(result.shaderUid, 0) << result.error;
		auto shader = m_editor.GetResourceManager().Get<neurus::RenderShader>(result.shaderUid);
		ASSERT_TRUE(shader);
		EXPECT_EQ(shader->GetName(), row.name);
		for (auto stage : { neurus::ShaderType::VERTEX, neurus::ShaderType::FRAGMENT })
		{
			ASSERT_TRUE(shader->HasStage(stage));
			EXPECT_FALSE(shader->GetStage(stage).code.empty());
		}
		EXPECT_TRUE(m_editor.IsDirty());
	}
}

TEST_F(ShaderCreateTest, LoadComputePoolsAComputeShader)
{
	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderImportRequested{
		"res/shaders/compute/dummy.comp.glsl", "MyCompute", &result});
	ASSERT_NE(result.shaderUid, 0) << result.error;
	auto shader = m_editor.GetResourceManager().Get<neurus::ComputeShader>(result.shaderUid);
	ASSERT_TRUE(shader);
	EXPECT_EQ(shader->GetType(), neurus::ShaderType::COMPUTE);
	EXPECT_FALSE(shader->GetStage(neurus::ShaderType::COMPUTE).code.empty());
}

TEST_F(ShaderCreateTest, LoadRejectsUnknownExtensionAndGeometry)
{
	for (const std::string path : {"res/shaders/render/gbuffer.txt",
	                               "res/shaders/render/gbuffer.geom.glsl"})
	{
		neurus::ShaderRequestResult result;
		m_editor.GetEventQueue().Emit(neurus::ShaderImportRequested{path, "", &result});
		EXPECT_EQ(result.shaderUid, 0) << path;
		EXPECT_FALSE(result.error.empty()) << path;
	}
}

TEST_F(ShaderCreateTest, LoadedNameSurvivesResourceComponentRoundTrip)
{
	neurus::ShaderRequestResult loaded;
	m_editor.GetEventQueue().Emit(neurus::ShaderImportRequested{
		"res/shaders/render/gbuffer.frag.glsl", "MyFrag", &loaded});
	ASSERT_NE(loaded.shaderUid, 0) << loaded.error;

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		neurus::project::ResourceComponent save(m_editor.GetResourceManager(), m_editor.GetRenderManager().Defaults());
		save.Save(out);
	}
	neurus::ResourceManager restored;
	{
		cereal::JSONInputArchive in(ss);
		neurus::project::ResourceComponent load(restored, m_editor.GetRenderManager().Defaults());
		load.Load(in);
	}

	EXPECT_TRUE(restored.GetByName<neurus::RenderShader>("MyFrag"));
}

TEST_F(ShaderCreateTest, ShaderCreateEventCreatesAndLinksDefaultShader)
{
	EXPECT_FALSE(m_editor.IsDirty());

	m_editor.GetEventQueue().Emit(neurus::ShaderCreateEvent{ GroundUid() });

	auto mesh = m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid());
	ASSERT_TRUE(mesh->o_shader);
	EXPECT_FALSE(mesh->o_shader->GetStage(neurus::ShaderType::VERTEX).spv.empty());
	EXPECT_FALSE(mesh->o_shader->GetStage(neurus::ShaderType::FRAGMENT).spv.empty());
	EXPECT_TRUE(m_editor.IsDirty());
}

TEST_F(ShaderCreateTest, CreateFromPooledShaderCopiesTheMatchingStage)
{
	neurus::ShaderRequestResult loaded;
	m_editor.GetEventQueue().Emit(neurus::ShaderImportRequested{
		"res/shaders/render/gbuffer.frag.glsl", "MyFrag", &loaded});
	ASSERT_NE(loaded.shaderUid, 0) << loaded.error;
	auto source = m_editor.GetResourceManager().Get<neurus::RenderShader>(loaded.shaderUid);
	ASSERT_TRUE(source);

	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "", "", 0, loaded.shaderUid, &result});
	ASSERT_NE(result.shaderUid, 0) << result.error;

	auto mesh = m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid());
	ASSERT_TRUE(mesh->o_shader);
	ASSERT_NE(mesh->o_shader->FindStage(neurus::ShaderType::FRAGMENT), nullptr);
	EXPECT_EQ(mesh->o_shader->FindStage(neurus::ShaderType::FRAGMENT)->code,
	          source->FindStage(neurus::ShaderType::FRAGMENT)->code);
}

TEST_F(ShaderCreateTest, ReplaceIsOneUndoEntryAndRestoresTheOldShader)
{
	neurus::ShaderRequestResult first;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "", "", 0, 0, &first});
	ASSERT_NE(first.shaderUid, 0) << first.error;

	auto mesh = m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid());
	ASSERT_TRUE(mesh->o_shader);
	EXPECT_FALSE(mesh->o_shader->GetStage(neurus::ShaderType::VERTEX).spv.empty());
	EXPECT_FALSE(mesh->o_shader->GetStage(neurus::ShaderType::FRAGMENT).spv.empty());
	EXPECT_TRUE(m_editor.IsDirty());

	neurus::ShaderRequestResult second;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "res/shaders/render/gbuffer.vert.glsl", "res/shaders/render/gbuffer.frag.glsl", 0, 0, &second});
	ASSERT_NE(second.shaderUid, 0) << second.error;
	ASSERT_NE(second.shaderUid, first.shaderUid);
	auto replacement = m_editor.GetResourceManager().Get<neurus::RenderShader>(second.shaderUid);
	ASSERT_TRUE(replacement);
	EXPECT_EQ(replacement->GetFragPath(), "res/shaders/render/gbuffer.frag.glsl");

	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader.id(),
	          second.shaderUid);
	EXPECT_TRUE(m_editor.GetResourceManager().Get<neurus::RenderShader>(first.shaderUid));

	m_editor.GetEventQueue().Emit(neurus::UndoRequested{});
	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader.id(),
	          first.shaderUid);

	m_editor.GetEventQueue().Emit(neurus::RedoRequested{});
	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader.id(),
	          second.shaderUid);
}

TEST_F(ShaderCreateTest, FailedCreateLeavesTheMeshUntouchedAndThePoolClean)
{
	std::size_t before = 0;
	m_editor.GetResourceManager().ForEachUID(
		[&](const std::shared_ptr<neurus::UID>&) { ++before; });

	neurus::ShaderCreateRequested request;
	request.objectUid = GroundUid();
	request.fragmentPath = "res/shaders/render/does_not_exist.frag.glsl";
	neurus::ShaderRequestResult result;
	request.result = &result;
	m_editor.GetEventQueue().Emit(request);

	EXPECT_EQ(result.shaderUid, 0);
	EXPECT_FALSE(result.error.empty());
	EXPECT_FALSE(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader);

	std::size_t after = 0;
	m_editor.GetResourceManager().ForEachUID(
		[&](const std::shared_ptr<neurus::UID>&) { ++after; });
	EXPECT_EQ(after, before);
}

TEST_F(ShaderCreateTest, RemoveDropsTheReferenceAndUndoRelinks)
{
	neurus::ShaderRequestResult created;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "", "", 0, 0, &created});
	ASSERT_NE(created.shaderUid, 0) << created.error;

	m_editor.ClearDirty();
	m_editor.GetEventQueue().Emit(neurus::ShaderRemoveRequested{GroundUid()});
	EXPECT_TRUE(m_editor.IsDirty());
	EXPECT_FALSE(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader);
	EXPECT_TRUE(m_editor.GetResourceManager().Get<neurus::RenderShader>(created.shaderUid));

	m_editor.GetEventQueue().Emit(neurus::UndoRequested{});
	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader.id(),
	          created.shaderUid);
}

TEST_F(ShaderCreateTest, CompileFailureDuringCreateLeavesPoolAndMeshUntouched)
{
	neurus::ShaderRequestResult loaded;
	m_editor.GetEventQueue().Emit(neurus::ShaderImportRequested{
		"res/shaders/render/gbuffer.frag.glsl", "BadFrag", &loaded});
	ASSERT_NE(loaded.shaderUid, 0) << loaded.error;
	auto source = m_editor.GetResourceManager().Get<neurus::RenderShader>(loaded.shaderUid);
	ASSERT_TRUE(source);
	source->GetStage(neurus::ShaderType::FRAGMENT).code = "not glsl";

	std::size_t before = 0;
	m_editor.GetResourceManager().ForEachUID(
		[&](const std::shared_ptr<neurus::UID>&) { ++before; });

	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "", "", 0, loaded.shaderUid, &result});

	EXPECT_EQ(result.shaderUid, 0);
	EXPECT_FALSE(result.error.empty());
	EXPECT_FALSE(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader);

	std::size_t after = 0;
	m_editor.GetResourceManager().ForEachUID(
		[&](const std::shared_ptr<neurus::UID>&) { ++after; });
	EXPECT_EQ(after, before);
}
