/**
 * @file test_shader_data.cpp
 * @brief Persistence tests for the CPU-side shader data.
 *
 * Covers the ShaderStruct/ShaderUnit cereal round-trip, RenderShader edited
 * content and legacy path reload, ComputeShader content/defaults, and the
 * pooled shader name re-seed performed by ResourceComponent::Load.
 */

#include <gtest/gtest.h>

#include <cereal/archives/json.hpp>

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

#include "asset/components/ResourceComponent.h"
#include "asset/data/shader/ShaderData.h"
#include "asset/data/shader/ShaderStruct.h"
#include "core/ResourceManager.h"
#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"

namespace
{

constexpr char kMinimalVertex[] =
	"#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
constexpr char kMinimalFragment[] =
	"#version 450\nlayout(location=0) out vec4 o;\nvoid main() { o = vec4(1.0); }\n";

} // anonymous namespace

TEST(ShaderDataPersistence, ShaderStructRoundTripsEveryContainer)
{
	neurus::ShaderStruct src;
	src.SetAB(0, neurus::ParaType::VEC3, "a_position");
	src.SetUB("Ubo", "ubo", {{neurus::ParaType::FLOAT, "u_time", ""}}, 3);
	src.SetSB(1, "Sbo", {{neurus::ParaType::FLOAT, "g_value", ""}});
	src.DefStruct("Light", {{neurus::ParaType::VEC3, "color", ""}});
	src.SetUni(neurus::ParaType::FLOAT, 1, "u_roughness", 4);
	src.SetInp(neurus::ParaType::VEC2, 1, "v_uv");
	src.SetOut(neurus::ParaType::VEC4, 1, "o_color");
	src.SetConst(neurus::ParaType::FLOAT, "PI", "3.14");
	src.SetGlob(neurus::ParaType::FLOAT, 0.5f, "g_gain");
	src.SetVar("float", "local", 1);
	src.DefFunc(neurus::ParaType::FLOAT, "helper", "return 1.0;", {});
	src.Main = "o_color = vec4(1.0);";
	src.SetPushConstant("pc_x", 0, 4, "float");
	src.SetLocalSize(8, 4, 1);
	src.AddExtension("GL_EXT_debug_printf");
	src.AddDefine("#define N 4");
	src.SetVersion(450);

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("ir", src));
	}
	neurus::ShaderStruct dst;
	{
		cereal::JSONInputArchive in(ss);
		in(cereal::make_nvp("ir", dst));
	}

	EXPECT_EQ(dst.AB_list.size(), 1u);
	EXPECT_EQ(dst.ubuffer_list.at(0).binding, 3);
	EXPECT_EQ(dst.uniform_list.at(0).name, "u_roughness");
	EXPECT_EQ(dst.func_list.at(0).name, "helper");
	EXPECT_EQ(dst.Main, "o_color = vec4(1.0);");
	EXPECT_EQ(dst.push_constants.at(0).offset, 0u);
	EXPECT_EQ(dst.local_size_x, 8u);
	EXPECT_EQ(dst.version, 450);
}

TEST(ShaderDataPersistence, ShaderUnitDoesNotPersistSpvOrVersion)
{
	neurus::ShaderUnit unit;
	unit.path = "res/x.frag";
	unit.code = "#version 450\nvoid main() {}\n";
	unit.spv = {0x07230203u};
	unit.BumpVersion();

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("unit", unit));
	}
	neurus::ShaderUnit dst;
	{
		cereal::JSONInputArchive in(ss);
		in(cereal::make_nvp("unit", dst));
	}

	EXPECT_EQ(dst.path, unit.path);
	EXPECT_EQ(dst.code, unit.code);
	EXPECT_TRUE(dst.spv.empty());
	EXPECT_EQ(dst.GetVersion(), 0);
}

TEST(ShaderDataPersistence, RenderShaderKeepsEditedContent)
{
	neurus::RenderShader src("Edited", "v.vert", "f.frag");
	src.GetStage(neurus::ShaderType::VERTEX).code = kMinimalVertex;
	src.GetStage(neurus::ShaderType::FRAGMENT).code = kMinimalFragment;
	src.GetStage(neurus::ShaderType::FRAGMENT).parsed.SetUni(
		neurus::ParaType::FLOAT, 1, "u_time", 2);

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("shader", src));
	}
	neurus::RenderShader dst("", "", "");
	{
		cereal::JSONInputArchive in(ss);
		in(cereal::make_nvp("shader", dst));
	}

	EXPECT_EQ(dst.GetStage(neurus::ShaderType::VERTEX).code, kMinimalVertex);
	ASSERT_EQ(dst.GetStage(neurus::ShaderType::FRAGMENT).parsed.uniform_list.size(), 1u);
	EXPECT_EQ(dst.GetStage(neurus::ShaderType::FRAGMENT).parsed.uniform_list[0].name, "u_time");
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::VERTEX).spv.empty());
}

TEST(ShaderDataPersistence, RenderShaderLegacyPathsReloadFromDisk)
{
	neurus::RenderShader src("Legacy", "res/shaders/render/gbuffer.vert",
	                         "res/shaders/render/gbuffer.frag");
	// No ParseAndGenerate: code is empty, so load must reload from the paths.
	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("shader", src));
	}
	neurus::RenderShader dst("", "", "");
	{
		cereal::JSONInputArchive in(ss);
		in(cereal::make_nvp("shader", dst));
	}
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::VERTEX).code.empty());
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::VERTEX).spv.empty());
}

TEST(ShaderDataPersistence, LegacyMissingStagesReloadsFromPaths)
{
	neurus::RenderShader src("Legacy", "res/shaders/render/gbuffer.vert",
	                         "res/shaders/render/gbuffer.frag");

	std::stringstream saved;
	{
		cereal::JSONOutputArchive out(saved);
		out(cereal::make_nvp("shader", src));
	}

	// Strip the "stages" node to reproduce a project saved before shader
	// content was serialized. The absent key must fall back to the paths.
	auto doc = nlohmann::ordered_json::parse(saved.str());
	ASSERT_TRUE(doc["shader"].contains("stages"));
	doc["shader"].erase("stages");

	std::stringstream legacy(doc.dump());
	neurus::RenderShader dst("", "", "");
	{
		cereal::JSONInputArchive in(legacy);
		in(cereal::make_nvp("shader", dst));
	}

	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::VERTEX).code.empty());
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::VERTEX).spv.empty());
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::FRAGMENT).code.empty());
}

TEST(ShaderDataPersistence, ComputeShaderSurvivesResourceComponentRoundTrip)
{
	neurus::ResourceManager resources;
	auto compute = resources.Load<neurus::ComputeShader>(
		"Comp", "res/shaders/compute/dummy.comp");
	ASSERT_TRUE(compute->ParseAndGenerate());
	resources.SetName(compute->GetObjectID(), "Comp");
	compute->SetName("Comp");

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		neurus::project::ResourceComponent save(resources);
		save.Save(out);
	}
	neurus::ResourceManager restored;
	{
		cereal::JSONInputArchive in(ss);
		neurus::project::ResourceComponent load(restored);
		load.Load(in);
	}

	auto loaded = restored.GetByName<neurus::ComputeShader>("Comp");
	ASSERT_TRUE(loaded);
	EXPECT_EQ(loaded->GetType(), neurus::ShaderType::COMPUTE);
	EXPECT_FALSE(loaded->GetStage(neurus::ShaderType::COMPUTE).code.empty());
	EXPECT_FALSE(loaded->GetStage(neurus::ShaderType::COMPUTE).spv.empty());
}

TEST(ShaderDataPersistence, ComputeShaderRoundTripsContentAndDefaults)
{
	neurus::ComputeShader src("Dummy", "res/shaders/compute/dummy.comp");
	ASSERT_TRUE(src.ParseAndGenerate());
	src.SetDefault("radius", 0.5f);

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("shader", src));
	}
	neurus::ComputeShader dst;
	{
		cereal::JSONInputArchive in(ss);
		in(cereal::make_nvp("shader", dst));
	}
	EXPECT_EQ(dst.GetType(), neurus::ShaderType::COMPUTE);
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::COMPUTE).code.empty());
	ASSERT_EQ(dst.GetDefaults().size(), 1u);
	EXPECT_EQ(dst.GetDefaults()[0].name, "radius");
	EXPECT_FALSE(dst.GetStage(neurus::ShaderType::COMPUTE).spv.empty());
}

TEST(ShaderDataPersistence, ShaderNamesSurviveResourceComponentRoundTrip)
{
	neurus::ResourceManager resources;
	auto shader = resources.Load<neurus::RenderShader>(
		"MyFrag", "res/shaders/render/gbuffer.vert", "res/shaders/render/gbuffer.frag");
	resources.SetName(shader->GetObjectID(), "MyFrag");
	shader->SetName("MyFrag");

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		neurus::project::ResourceComponent save(resources);
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
