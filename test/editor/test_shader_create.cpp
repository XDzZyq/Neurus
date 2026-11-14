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

#include "asset/components/ResourceComponent.h"
#include "asset/data/shader/ShaderData.h"
#include "editor/Editor.h"
#include "editor/events/OperationEvents.h"
#include "editor/events/ShaderEvents.h"
#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

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

TEST_F(ShaderCreateTest, LoadFragmentBuildsRenderBundleWithDefaultVertex)
{
	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderLoadRequested{
		"res/shaders/render/gbuffer.frag", "", &result});
	ASSERT_NE(result.shaderUid, 0) << result.error;
	auto shader = m_editor.GetResourceManager().Get<neurus::RenderShader>(result.shaderUid);
	ASSERT_TRUE(shader);
	EXPECT_TRUE(shader->HasStage(neurus::ShaderType::VERTEX));
	EXPECT_TRUE(shader->HasStage(neurus::ShaderType::FRAGMENT));
	EXPECT_FALSE(shader->GetStage(neurus::ShaderType::FRAGMENT).code.empty());
	EXPECT_FALSE(shader->GetStage(neurus::ShaderType::VERTEX).code.empty());
}

TEST_F(ShaderCreateTest, LoadVertexUsesDefaultFragment)
{
	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderLoadRequested{
		"res/shaders/render/gbuffer.vert", "MyVert", &result});
	ASSERT_NE(result.shaderUid, 0) << result.error;
	auto shader = m_editor.GetResourceManager().Get<neurus::RenderShader>(result.shaderUid);
	ASSERT_TRUE(shader);
	EXPECT_FALSE(shader->GetStage(neurus::ShaderType::FRAGMENT).code.empty());
	EXPECT_EQ(shader->GetName(), "MyVert");
}

TEST_F(ShaderCreateTest, LoadComputePoolsAComputeShader)
{
	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderLoadRequested{
		"res/shaders/compute/dummy.comp", "MyCompute", &result});
	ASSERT_NE(result.shaderUid, 0) << result.error;
	auto shader = m_editor.GetResourceManager().Get<neurus::ComputeShader>(result.shaderUid);
	ASSERT_TRUE(shader);
	EXPECT_EQ(shader->GetType(), neurus::ShaderType::COMPUTE);
	EXPECT_FALSE(shader->GetStage(neurus::ShaderType::COMPUTE).code.empty());
}

TEST_F(ShaderCreateTest, LoadRejectsUnknownExtensionAndGeometry)
{
	for (const std::string path : {"res/shaders/render/gbuffer.txt",
	                               "res/shaders/render/gbuffer.geom"})
	{
		neurus::ShaderRequestResult result;
		m_editor.GetEventQueue().Emit(neurus::ShaderLoadRequested{path, "", &result});
		EXPECT_EQ(result.shaderUid, 0) << path;
		EXPECT_FALSE(result.error.empty()) << path;
	}
}

TEST_F(ShaderCreateTest, LoadedNameSurvivesResourceComponentRoundTrip)
{
	neurus::ShaderRequestResult loaded;
	m_editor.GetEventQueue().Emit(neurus::ShaderLoadRequested{
		"res/shaders/render/gbuffer.frag", "MyFrag", &loaded});
	ASSERT_NE(loaded.shaderUid, 0) << loaded.error;

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		neurus::project::ResourceComponent save(m_editor.GetResourceManager());
		save.Save(out);
	}
	neurus::ResourceManager restored;
	{
		cereal::JSONInputArchive in(ss);
		neurus::project::ResourceComponent load(restored);
		load.Load(in);
	}

	EXPECT_TRUE(restored.GetByName<neurus::RenderShader>("MyFrag"));
}

TEST_F(ShaderCreateTest, CreateDefaultBuildsAndCompilesBothStages)
{
	neurus::ShaderRequestResult result;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "", "", 0, 0, &result});
	ASSERT_NE(result.shaderUid, 0) << result.error;

	auto mesh = m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid());
	ASSERT_TRUE(mesh->o_shader);
	EXPECT_FALSE(mesh->o_shader->GetStage(neurus::ShaderType::VERTEX).spv.empty());
	EXPECT_FALSE(mesh->o_shader->GetStage(neurus::ShaderType::FRAGMENT).spv.empty());
}

TEST_F(ShaderCreateTest, CreateFromPathsUsesTheGivenSources)
{
	neurus::ShaderCreateRequested request;
	request.objectUid = GroundUid();
	request.vertexPath = "res/shaders/render/gbuffer.vert";
	request.fragmentPath = "res/shaders/render/gbuffer.frag";
	neurus::ShaderRequestResult result;
	request.result = &result;
	m_editor.GetEventQueue().Emit(request);
	ASSERT_NE(result.shaderUid, 0) << result.error;

	auto shader = m_editor.GetResourceManager().Get<neurus::RenderShader>(result.shaderUid);
	ASSERT_TRUE(shader);
	EXPECT_EQ(shader->GetFragPath(), request.fragmentPath);
}

TEST_F(ShaderCreateTest, CreateFromPooledShaderCopiesTheMatchingStage)
{
	neurus::ShaderRequestResult loaded;
	m_editor.GetEventQueue().Emit(neurus::ShaderLoadRequested{
		"res/shaders/render/gbuffer.frag", "MyFrag", &loaded});
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

	neurus::ShaderRequestResult second;
	m_editor.GetEventQueue().Emit(neurus::ShaderCreateRequested{
		GroundUid(), "", "", 0, 0, &second});
	ASSERT_NE(second.shaderUid, 0) << second.error;
	ASSERT_NE(second.shaderUid, first.shaderUid);

	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shaderId,
	          second.shaderUid);
	EXPECT_TRUE(m_editor.GetResourceManager().Get<neurus::RenderShader>(first.shaderUid));

	m_editor.GetEventQueue().Emit(neurus::UndoRequested{});
	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shaderId,
	          first.shaderUid);

	m_editor.GetEventQueue().Emit(neurus::RedoRequested{});
	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shaderId,
	          second.shaderUid);
}

TEST_F(ShaderCreateTest, FailedCreateLeavesTheMeshUntouchedAndThePoolClean)
{
	std::size_t before = 0;
	m_editor.GetResourceManager().ForEachUID(
		[&](const std::shared_ptr<neurus::UID>&) { ++before; });

	neurus::ShaderCreateRequested request;
	request.objectUid = GroundUid();
	request.fragmentPath = "res/shaders/render/does_not_exist.frag";
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

	m_editor.GetEventQueue().Emit(neurus::ShaderRemoveRequested{GroundUid()});
	EXPECT_FALSE(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shader);
	EXPECT_TRUE(m_editor.GetResourceManager().Get<neurus::RenderShader>(created.shaderUid));

	m_editor.GetEventQueue().Emit(neurus::UndoRequested{});
	EXPECT_EQ(m_editor.GetResourceManager().Get<neurus::Mesh>(GroundUid())->o_shaderId,
	          created.shaderUid);
}
