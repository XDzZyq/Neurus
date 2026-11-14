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
#include "editor/events/ShaderEvents.h"
#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"

class ShaderCreateTest : public ::testing::Test
{
protected:
	void SetUp() override { m_editor.Initialize(); }

	neurus::Editor m_editor{nullptr};
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
