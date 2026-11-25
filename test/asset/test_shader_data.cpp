/**
 * @file test_shader_data.cpp
 * @brief Persistence tests for the CPU-side shader data.
 *
 * Covers the ShaderStruct/ShaderUnit cereal round-trip, RenderShader edited
 * content and source-backed stage reload, ComputeShader content/defaults, and the
 * pooled shader name re-seed performed by ResourceComponent::Load.
 */

#include <gtest/gtest.h>

#include <cereal/archives/json.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <string>

#include "shader/BuiltInRenderData.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/Project.hpp"
#include "asset/data/MeshData.hpp"
#include "shader/registrations/ShaderRegistration.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/shader/ShaderStruct.hpp"
#include "asset/ResourceManager.hpp"
#include "scene/Mesh.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/RenderShader.hpp"

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
	unit.path = "res/x.frag.glsl";
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
	neurus::RenderShader src("Edited", "v.vert.glsl", "f.frag.glsl");
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

TEST(ShaderDataPersistence, EmptyRenderStageCodeReloadsFromDisk)
{
	neurus::RenderShader src("Legacy", "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
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

TEST(ShaderDataPersistence, MissingStageMapIsRejected)
{
	neurus::RenderShader src("Legacy", "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");

	std::stringstream saved;
	{
		cereal::JSONOutputArchive out(saved);
		out(cereal::make_nvp("shader", src));
	}

	auto doc = nlohmann::ordered_json::parse(saved.str());
	ASSERT_TRUE(doc["shader"].contains("stages"));
	doc["shader"].erase("stages");

	std::stringstream legacy(doc.dump());
	neurus::RenderShader dst("", "", "");
	cereal::JSONInputArchive in(legacy);
	EXPECT_THROW(in(cereal::make_nvp("shader", dst)), cereal::Exception);
}

TEST(ShaderDataPersistence, ResourceComponentPreservesRenderAndComputeShadersByName)
{

	neurus::ResourceManager resources;
	auto compute = resources.Load<neurus::ComputeShader>(
		"Comp", "res/shaders/compute/dummy.comp.glsl");
	ASSERT_TRUE(compute->ParseAndGenerate());
	resources.SetName(compute->GetObjectID(), "Comp");
	compute->SetName("Comp");

	compute->SetDefault("radius", 0.5f);
	auto shader = resources.Load<neurus::RenderShader>(
		"MyFrag", "res/shaders/render/gbuffer.vert.glsl", "res/shaders/render/gbuffer.frag.glsl");
	resources.SetName(shader->GetObjectID(), "MyFrag");
	shader->SetName("MyFrag");

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		neurus::project::ResourceComponent save(resources);
		save.Save(out);
	}
	// Both shader types persist their name and concrete shader fields.
	const auto saved = nlohmann::ordered_json::parse(ss.str());
	for (const auto& entry : saved.at("m_resources").at("resources_"))
	{
		const auto& data = entry.at("data");
		if (entry.at("key") == compute->GetObjectID())
		{
			EXPECT_EQ(data.at("value0").at("m_name"), "Comp");
			EXPECT_EQ(data.at("m_compPath"), "res/shaders/compute/dummy.comp.glsl");
			EXPECT_EQ(data.at("m_defaults").at(0).at("name"), "radius");
		}
		if (entry.at("key") == shader->GetObjectID())
		{
			EXPECT_EQ(data.at("value0").at("m_name"), "MyFrag");
			EXPECT_EQ(data.at("m_vertPath"), "res/shaders/render/gbuffer.vert.glsl");
			EXPECT_EQ(data.at("m_fragPath"), "res/shaders/render/gbuffer.frag.glsl");
		}
	}
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(ss.str());

	auto loaded = restored.GetByName<neurus::ComputeShader>("Comp");
	ASSERT_TRUE(loaded);
	EXPECT_EQ(loaded->GetType(), neurus::ShaderType::COMPUTE);
	EXPECT_FALSE(loaded->GetStage(neurus::ShaderType::COMPUTE).code.empty());
	EXPECT_FALSE(loaded->GetStage(neurus::ShaderType::COMPUTE).spv.empty());
	ASSERT_EQ(loaded->GetDefaults().size(), 1u);
	EXPECT_EQ(loaded->GetDefaults()[0].name, "radius");
	EXPECT_TRUE(restored.GetByName<neurus::RenderShader>("MyFrag"));
}

TEST(ShaderDataPersistence, FindStageReturnsNullptrWhenStageIsAbsent)
{
	neurus::RenderShader source("Source", "res/shaders/render/gbuffer.vert.glsl",
	                            "res/shaders/render/gbuffer.frag.glsl");
	source.GetStage(neurus::ShaderType::VERTEX).code = "VERT";
	source.GetStage(neurus::ShaderType::FRAGMENT).code = "FRAG";

	const neurus::ShaderUnit* frag = source.FindStage(neurus::ShaderType::FRAGMENT);
	ASSERT_NE(frag, nullptr);
	EXPECT_EQ(frag->code, "FRAG");

	// A compute bundle has no vertex stage: FindStage never default-inserts.
	EXPECT_EQ(source.FindStage(neurus::ShaderType::COMPUTE), nullptr);
}

TEST(ShaderDataPersistence, CompiledReadinessRequiresEveryRenderStage)
{
	neurus::RenderShader shader("Stages");
	EXPECT_FALSE(shader.IsValid());
	shader.GetVertex().code = kMinimalVertex;
	shader.GetFragment().code = kMinimalFragment;
	ASSERT_TRUE(shader.CompileToSpv());
	EXPECT_TRUE(shader.IsValid());
	shader.GetFragment().spv.clear();
	EXPECT_FALSE(shader.IsValid());
}

TEST(ShaderDataPersistence, FailedShaderLoadClearsMeshReferenceAndIsOmittedOnSave)
{

	neurus::ResourceManager resources;
	auto broken = resources.Load<neurus::RenderShader>("Broken");
	broken->GetVertex().code = kMinimalVertex;
	broken->GetFragment().code = "this is not valid GLSL";
	auto mesh = resources.Load<neurus::Mesh>();
	mesh->SetObjShader(broken);
	auto good = resources.Load<neurus::ComputeShader>(
		"Good", "res/shaders/compute/dummy.comp.glsl");
	const int meshId = mesh->GetObjectID();
	const int brokenId = broken->GetObjectID();
	const int goodId = good->GetObjectID();
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ResourceComponent save(resources);
		save.Save(archive);
	}

	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(serialized.str());
	EXPECT_FALSE(restored.Get(brokenId));
	EXPECT_FALSE(restored.Contains(brokenId));
	const auto restoredMesh = restored.Get<neurus::Mesh>(meshId);
	ASSERT_NE(restoredMesh, nullptr);
	EXPECT_FALSE(restoredMesh->o_shader);
	EXPECT_EQ(restoredMesh->o_shader.id(), 0);
	const auto restoredGood = restored.Get<neurus::ShaderData>(goodId);
	ASSERT_NE(restoredGood, nullptr);
	EXPECT_TRUE(restoredGood->IsValid());

	std::stringstream savedAgain;
	{
		cereal::JSONOutputArchive archive(savedAgain);
		neurus::project::ResourceComponent save(restored);
		save.Save(archive);
	}
	EXPECT_EQ(savedAgain.str().find("Broken"), std::string::npos);
	EXPECT_EQ(savedAgain.str().find("o_shaderId"), std::string::npos);
	EXPECT_NE(savedAgain.str().find("Good"), std::string::npos);
}

TEST(ShaderDataPersistence, MissingShaderSourceDoesNotCreatePersistedFallback)
{

	neurus::ResourceManager resources;
	auto shader = resources.Load<neurus::RenderShader>(
		"Unavailable", "missing-default-fallback.vert.glsl", "missing-default-fallback.frag.glsl");
	auto mesh = resources.Load<neurus::Mesh>();
	mesh->SetObjShader(shader);
	const int shaderId = shader->GetObjectID();
	const int meshId = mesh->GetObjectID();
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ResourceComponent save(resources);
		save.Save(archive);
	}
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(serialized.str());
	EXPECT_FALSE(restored.Get(shaderId));
	EXPECT_FALSE(restored.Contains(shaderId));
	const auto restoredMesh = restored.Get<neurus::Mesh>(meshId);
	ASSERT_NE(restoredMesh, nullptr);
	EXPECT_FALSE(restoredMesh->o_shader);
	EXPECT_EQ(restoredMesh->o_shader.id(), 0);
	int shaderCount = 0;
	restored.ForEachUID([&shaderCount](const std::shared_ptr<neurus::UID>& resource)
	{
		if (std::dynamic_pointer_cast<neurus::ShaderData>(resource))
			++shaderCount;
	});
	EXPECT_EQ(shaderCount, 0);
}

TEST(ShaderDataPersistence, SavingLiveCompileFailurePreservesEditableSource)
{

	neurus::ResourceManager resources;
	auto shader = resources.Load<neurus::RenderShader>("Editable");
	shader->GetVertex().code = kMinimalVertex;
	shader->GetFragment().code = kMinimalFragment;
	ASSERT_TRUE(shader->CompileToSpv());
	shader->GetFragment().code = "incomplete live edit";
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ResourceComponent save(resources);
		save.Save(archive);
	}
	EXPECT_TRUE(resources.Contains(shader->GetObjectID()));
	EXPECT_NE(serialized.str().find("incomplete live edit"), std::string::npos);
}

TEST(ShaderDataPersistence, NullPoolEntriesDoNotSurviveLoadOrSave)
{

	neurus::ResourceManager restored;
	std::stringstream serialized(
		R"({"m_resources":{"resources_":[{"key":9123,"type":"neurus::RenderShader","data":null}]}})");
	neurus::project::ResourceComponent(restored).LoadJson(serialized.str());
	EXPECT_EQ(restored.Size(), 0u);
	std::stringstream saved;
	{
		cereal::JSONOutputArchive archive(saved);
		neurus::project::ResourceComponent save(restored);
		save.Save(archive);
	}
	EXPECT_EQ(saved.str().find("9123"), std::string::npos);
}

TEST(ShaderDataPersistence, ConcurrentCompilationKeepsDiagnosticsWithTheirSource)
{
	constexpr int rounds = 8;
	std::barrier ready(3);
	std::array<std::array<std::string, rounds>, 2> errors;
	std::array<std::array<bool, rounds>, 2> failed{};
	std::array<bool, rounds> valid{};
	const auto compileFailure = [&](int worker)
	{
		for (int round = 0; round < rounds; ++round)
		{
			neurus::ShaderUnit stage;
			stage.code = "#version 450\nvoid main() { unknown_symbol(); }\n";
			const std::string name = "thread-source-" + std::to_string(worker);
			ready.arrive_and_wait();
			const auto spv = neurus::ShaderLibrary::Compile(
				stage, neurus::ShaderType::VERTEX, name, &errors[worker][round]);
			failed[worker][round] = spv.empty();
		}
	};
	std::thread first(compileFailure, 0);
	std::thread second(compileFailure, 1);
	for (int round = 0; round < rounds; ++round)
	{
		neurus::ShaderUnit stage;
		stage.code = kMinimalVertex;
		ready.arrive_and_wait();
		const auto spv = neurus::ShaderLibrary::Compile(
			stage, neurus::ShaderType::VERTEX, "thread-valid-source");
		valid[round] = !spv.empty() && spv.front() == 0x07230203u;
	}
	first.join();
	second.join();

	for (int round = 0; round < rounds; ++round)
	{
		EXPECT_TRUE(valid[round]);
		for (int worker = 0; worker < 2; ++worker)
		{
			EXPECT_TRUE(failed[worker][round]);
			EXPECT_NE(errors[worker][round].find("thread-source-" + std::to_string(worker)),
			          std::string::npos) << errors[worker][round];
		}
	}
}

TEST(ShaderDataPersistence, MalformedShaderEntryDoesNotDiscardOtherProjectResources)
{

	// Resource references reserve zero for absence; UID's first allocation is zero.
	neurus::UID reserved;
	neurus::ResourceManager resources;
	auto geometry = resources.Load<neurus::MeshData>("res/obj/plane.obj");
	ASSERT_NE(geometry->GetObjectID(), 0);
	ASSERT_GT(geometry->GetVertexCount(), 0u);
	auto first = resources.Load<neurus::RenderShader>("First");
	auto second = resources.Load<neurus::RenderShader>("Second");
	for (const auto& shader : {first, second})
	{
		shader->GetVertex().code = kMinimalVertex;
		shader->GetFragment().code = kMinimalFragment;
	}
	auto mesh = resources.Load<neurus::Mesh>(geometry);
	auto otherMesh = resources.Load<neurus::Mesh>(geometry);
	mesh->SetObjShader(first);
	otherMesh->SetObjShader(first);

	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto path = std::filesystem::temp_directory_path()
		/ ("neurus-isolated-resource-" + std::to_string(stamp) + ".json");
	struct Cleanup
	{
		std::filesystem::path path;
		~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
	} cleanup{path};
	neurus::project::Project source;
	source.Register<neurus::project::ResourceComponent>(resources);
	source.Save(path.string());
	nlohmann::ordered_json document;
	{ std::ifstream file(path); file >> document; }
	auto& entries = document["project"]["m_resources"]["resources_"];
	int brokenId = 0;
	for (auto& entry : entries)
	{
		if (entry.at("type") == "neurus::RenderShader")
		{
			// A malformed shader must leave its sibling of the same type loadable.
			brokenId = entry["key"].get<int>();
			entry["data"] = nlohmann::ordered_json::object();
			break;
		}
	}
	ASSERT_NE(brokenId, 0);
	for (auto& entry : entries)
	{
		if (entry["key"] == mesh->GetObjectID() || entry["key"] == otherMesh->GetObjectID())
			entry["data"]["o_shaderId"] = brokenId;
	}
	{ std::ofstream file(path); file << document.dump(); }

	neurus::ResourceManager restored;
	neurus::project::Project loaded;
	loaded.Register<neurus::project::ResourceComponent>(restored);
	ASSERT_NO_THROW(loaded.Load(path.string()));
	EXPECT_EQ(restored.Size(), 4u);
	EXPECT_FALSE(restored.Get(brokenId));
	EXPECT_FALSE(restored.Contains(brokenId));
	const int goodId = brokenId == first->GetObjectID() ? second->GetObjectID() : first->GetObjectID();
	ASSERT_NE(restored.Get<neurus::ShaderData>(goodId), nullptr);
	EXPECT_TRUE(restored.Get<neurus::ShaderData>(goodId)->IsValid());
	ASSERT_NE(restored.Get<neurus::MeshData>(geometry->GetObjectID()), nullptr);
	EXPECT_GT(restored.Get<neurus::MeshData>(geometry->GetObjectID())->GetVertexCount(), 0u);
	for (const int id : {mesh->GetObjectID(), otherMesh->GetObjectID()})
	{
		const auto restoredMesh = restored.Get<neurus::Mesh>(id);
		ASSERT_NE(restoredMesh, nullptr);
		EXPECT_FALSE(restoredMesh->o_shader);
		EXPECT_EQ(restoredMesh->o_shader.id(), 0);
		EXPECT_EQ(restoredMesh->o_mesh.shared(), restored.Get<neurus::MeshData>(geometry->GetObjectID()));
	}
	loaded.Save(path.string());
	std::ifstream file(path);
	const std::string saved((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	EXPECT_EQ(saved.find("o_shaderId"), std::string::npos);
}

TEST(ShaderDataPersistence, ComputeShaderRemainsPooledButCannotBindToAMesh)
{

	neurus::ResourceManager resources;
	auto compute = resources.Load<neurus::ComputeShader>(
		"Standalone", "res/shaders/compute/dummy.comp.glsl");
	auto mesh = resources.Load<neurus::Mesh>();
	mesh->SetObjShader(compute);
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ResourceComponent save(resources);
		save.Save(archive);
	}
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(serialized.str());
	ASSERT_NE(restored.Get<neurus::ComputeShader>(compute->GetObjectID()), nullptr);
	EXPECT_TRUE(restored.Get<neurus::ComputeShader>(compute->GetObjectID())->IsValid());
	const auto restoredMesh = restored.Get<neurus::Mesh>(mesh->GetObjectID());
	ASSERT_NE(restoredMesh, nullptr);
	EXPECT_FALSE(restoredMesh->o_shader);
	EXPECT_EQ(restoredMesh->o_shader.id(), 0);
}

TEST(ShaderDataPersistence, WrongShaderPrimitiveTypeIsIsolatedBeforeArchiveRead)
{

	neurus::ResourceManager resources;
	auto shader = resources.Load<neurus::RenderShader>("Malformed");
	shader->GetVertex().code = kMinimalVertex;
	shader->GetFragment().code = kMinimalFragment;
	auto mesh = resources.Load<neurus::Mesh>();
	mesh->SetObjShader(shader);
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ResourceComponent save(resources);
		save.Save(archive);
	}
	auto document = nlohmann::ordered_json::parse(serialized.str());
	for (auto& entry : document["m_resources"]["resources_"])
	{
		if (entry["key"] == shader->GetObjectID())
			entry["data"]["m_vertPath"] = 42;
	}
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent load(restored);
	ASSERT_NO_THROW(load.LoadJson(document.dump()));
	EXPECT_FALSE(restored.Get(shader->GetObjectID()));
	EXPECT_FALSE(restored.Contains(shader->GetObjectID()));
	const auto restoredMesh = restored.Get<neurus::Mesh>(mesh->GetObjectID());
	ASSERT_NE(restoredMesh, nullptr);
	EXPECT_FALSE(restoredMesh->o_shader);
	EXPECT_EQ(restoredMesh->o_shader.id(), 0);
}

TEST(ShaderInterface, IndependentlyCompiledStagesWithMissingOutputAreRejected)
{
	neurus::RenderShader shader("MissingOutput");
	shader.GetVertex().code = kMinimalVertex;
	shader.GetFragment().code =
		"#version 450\nlayout(location=0) in vec3 color;\n"
		"layout(location=0) out vec4 o;\nvoid main(){o=vec4(color,1);}";
	// Both modules are valid in isolation: only the cross-stage contract fails.
	shader.GetVertex().spv = neurus::ShaderLibrary::Compile(
		shader.GetVertex(), neurus::ShaderType::VERTEX, "MissingOutput.vert");
	shader.GetFragment().spv = neurus::ShaderLibrary::Compile(
		shader.GetFragment(), neurus::ShaderType::FRAGMENT, "MissingOutput.frag");
	ASSERT_FALSE(shader.GetVertex().spv.empty());
	ASSERT_FALSE(shader.GetFragment().spv.empty());
	std::string error;
	EXPECT_FALSE(shader.CompileToSpv(&error));
	EXPECT_NE(error.find("location 0"), std::string::npos);
	EXPECT_EQ(shader.GetErrorMessage(), error);
	EXPECT_TRUE(shader.GetVertex().spv.empty());
	EXPECT_TRUE(shader.GetFragment().spv.empty());
	EXPECT_FALSE(shader.IsValid());
}

TEST(ShaderInterface, ScalarTypeMismatchIsRejected)
{
	neurus::RenderShader shader("TypeMismatch");
	shader.GetVertex().code =
		"#version 450\nlayout(location=2) out vec3 color;\n"
		"void main(){gl_Position=vec4(0);color=vec3(1);}";
	shader.GetFragment().code =
		"#version 450\nlayout(location=2) flat in ivec3 color;\n"
		"layout(location=0) out vec4 o;\nvoid main(){o=vec4(color,1);}";
	std::string error;
	EXPECT_FALSE(shader.CompileToSpv(&error));
	EXPECT_NE(error.find("location 2"), std::string::npos);
	EXPECT_TRUE(shader.GetVertex().spv.empty());
	EXPECT_TRUE(shader.GetFragment().spv.empty());
}

TEST(ShaderInterface, BlocksArraysMatricesAndPackedComponentsMatch)
{
	neurus::RenderShader shader("ComplexInterface");
	shader.GetVertex().code =
		"#version 450\nlayout(location=0) out Data {mat2 m;vec3 colors[2];} data;\n"
		"layout(location=4,component=0) out vec2 first;\n"
		"layout(location=4,component=2) out vec2 second;\n"
		"layout(location=5) out vec3 unused;\n"
		"void main(){gl_Position=vec4(0);data.m=mat2(1);data.colors[0]=vec3(1);"
		"data.colors[1]=vec3(1);first=vec2(1);second=vec2(1);unused=vec3(1);}";
	shader.GetFragment().code =
		"#version 450\nlayout(location=0) in Data {mat2 m;vec3 colors[2];} data;\n"
		"layout(location=4,component=0) in vec2 first;\n"
		"layout(location=4,component=2) in vec2 second;\n"
		"layout(location=0) out vec4 o;\n"
		"void main(){o=vec4(data.colors[0]+data.colors[1],1)+vec4(data.m[0],first.x,second.y)"
		"+gl_FragCoord;}";
	std::string error;
	EXPECT_TRUE(shader.CompileToSpv(&error)) << error;
	EXPECT_TRUE(shader.IsValid());
	EXPECT_TRUE(error.empty());
}

TEST(ShaderDataPersistence, IncompatibleCompiledStagesAreDiscardedOnLoad)
{

	neurus::UID reserved;
	neurus::ResourceManager resources;
	auto shader = resources.Load<neurus::RenderShader>("Incompatible");
	shader->GetVertex().code = kMinimalVertex;
	shader->GetFragment().code =
		"#version 450\nlayout(location=0) in vec3 color;\n"
		"layout(location=0) out vec4 o;\nvoid main(){o=vec4(color,1);}";
	auto mesh = resources.Load<neurus::Mesh>();
	mesh->SetObjShader(shader);
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ResourceComponent save(resources);
		save.Save(archive);
	}
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(serialized.str());
	EXPECT_FALSE(restored.Get(shader->GetObjectID()));
	EXPECT_FALSE(restored.Contains(shader->GetObjectID()));
	const auto restoredMesh = restored.Get<neurus::Mesh>(mesh->GetObjectID());
	ASSERT_NE(restoredMesh, nullptr);
	EXPECT_FALSE(restoredMesh->o_shader);
	EXPECT_EQ(restoredMesh->o_shader.id(), 0);
	std::stringstream saved;
	{
		cereal::JSONOutputArchive archive(saved);
		neurus::project::ResourceComponent save(restored);
		save.Save(archive);
	}
	EXPECT_EQ(saved.str().find("Incompatible"), std::string::npos);
	EXPECT_EQ(saved.str().find("o_shaderId"), std::string::npos);
}
