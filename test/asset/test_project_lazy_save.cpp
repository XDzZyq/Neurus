#include "asset/Project.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/ResourceManager.hpp"
#include "scene/Camera.hpp"
#include "scene/Mesh.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/RenderShader.hpp"
#include "shader/registrations/ShaderRegistration.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace
{
using namespace neurus;
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

TEST(ProjectRoundtrip, MeshShaderReferenceRoundTripsAsScalarUid)
{

	ResourceManager source;
	const auto first = source.Load<Mesh>();
	const auto second = source.Load<Mesh>();
	const auto unbound = source.Load<Mesh>();
	const auto shader = source.Load<RenderShader>("Shared",
		ShaderData::ResolveSourcePath("res/shaders/render/gbuffer.vert.glsl"),
		ShaderData::ResolveSourcePath("res/shaders/render/gbuffer.frag.glsl"));
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	first->o_shader = shader;
	second->o_shader = shader;

	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	auto& records = document.at("m_resources").at("resources_");
	const int shaderId = shader->GetObjectID();
	auto shaderRecord = std::find_if(records.begin(), records.end(),
		[&](const Json& entry) { return entry.at("key") == shaderId; });
	ASSERT_NE(shaderRecord, records.end());
	// Place the shader after the meshes that reference it.
	std::rotate(shaderRecord, std::next(shaderRecord), records.end());
	for (auto record = records.begin(); record != std::prev(records.end()); ++record)
	{
		const auto& data = record->at("data");
		if (record->at("key") == unbound->GetObjectID())
			EXPECT_FALSE(data.contains("o_shaderId"));
		else
			EXPECT_EQ(data.at("o_shaderId"), shaderId);
	}
	const std::string saved = document.dump();
	auto check = [&](const ResourceManager& pool)
	{
		const auto a = pool.Get<Mesh>(first->GetObjectID());
		const auto b = pool.Get<Mesh>(second->GetObjectID());
		const auto c = pool.Get<Mesh>(unbound->GetObjectID());
		ASSERT_TRUE(a && b && c);
		ASSERT_NE(a->GetShader(), nullptr);
		EXPECT_EQ(a->GetShader(), b->GetShader());
		EXPECT_EQ(a->GetShader(), pool.Get<RenderShader>(shaderId).get());
		EXPECT_EQ(c->GetShader(), nullptr);
	};
	ResourceManager indexed;
	project::ResourceComponent(indexed).LoadJson(saved);
	EXPECT_FALSE(indexed.GetLive(shaderId));
	check(indexed);

}

TEST(ProjectRoundtrip, SaveAsPreservesLiveEditsAndUnloadedRecords)
{
	const fs::path directory = "mixed-pool-" +
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	ASSERT_TRUE(fs::create_directory(directory));
	struct Cleanup
	{
		fs::path directory;
		~Cleanup() { std::error_code ec; fs::remove_all(directory, ec); }
	} cleanup{directory};
	const auto original = directory / "original.neurus.json";
	const auto copy = directory / "copies" / "edited.neurus.json";
	fs::create_directory(copy.parent_path());
	const auto obj = directory / "triangle.obj";
	{
		std::ofstream file(obj);
		file << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
		ASSERT_TRUE(file);
	}

	int editedId, renamedId, removedLiveId, removedDeferredId, firstMeshId, secondMeshId, geometryId, plainId;
	{
		ResourceManager source;
		editedId = source.Load<Camera>()->GetObjectID();
		renamedId = source.Load<Camera>()->GetObjectID();
		removedLiveId = source.Load<Camera>()->GetObjectID();
		removedDeferredId = source.Load<Camera>()->GetObjectID();
		const auto geometry = source.Load<MeshData>(obj.generic_string());
		ASSERT_EQ(geometry->GetVertexCount(), 3u);
		geometryId = geometry->GetObjectID();
		const auto firstMesh = source.Load<Mesh>();
		firstMesh->o_mesh = geometry;
		firstMeshId = firstMesh->GetObjectID();
		secondMeshId = source.Load<Mesh>(geometry)->GetObjectID();
		plainId = source.Load<UID>()->GetObjectID();
		project::Project writer;
		writer.Register<project::ResourceComponent>(source);
		writer.Save(original.string());
	}
	ResourceManager pool;
	{
		project::Project reader;
		reader.Register<project::ResourceComponent>(pool);
		reader.Load(original.string());
	}
	ASSERT_TRUE(fs::remove(original));
	const auto edited = pool.Get<Camera>(editedId);
	ASSERT_TRUE(edited);
	edited->cam_pers = 37.0f;
	EXPECT_EQ(pool.SetName(renamedId, "Inspection camera"), "Inspection camera");
	ASSERT_TRUE(pool.Get<Camera>(removedLiveId));
	ASSERT_FALSE(pool.GetLive(removedDeferredId));
	ASSERT_TRUE(pool.Remove(removedLiveId));
	ASSERT_TRUE(pool.Remove(removedDeferredId));
	for (int id : {renamedId, firstMeshId, secondMeshId, geometryId, plainId})
		EXPECT_FALSE(pool.GetLive(id));
	{
		project::Project writer;
		writer.Register<project::ResourceComponent>(pool);
		writer.Save(copy.string());
	}
	for (int id : {renamedId, firstMeshId, secondMeshId, geometryId, plainId})
		EXPECT_FALSE(pool.GetLive(id));

	auto checkRestored = [&](const ResourceManager& restored)
	{
		ASSERT_EQ(restored.Size(), 6u);
		EXPECT_FALSE(restored.Contains(removedLiveId));
		EXPECT_FALSE(restored.Contains(removedDeferredId));
		const auto camera = restored.Get<Camera>(editedId);
		ASSERT_TRUE(camera);
		EXPECT_FLOAT_EQ(camera->cam_pers, 37.0f);
		const auto named = restored.GetByName<Camera>("Inspection camera");
		ASSERT_TRUE(named);
		EXPECT_EQ(named->o_name, "Inspection camera");
		EXPECT_EQ(named->GetObjectID(), renamedId);
		EXPECT_EQ(named, restored.Get<Camera>(renamedId));
		const auto first = restored.Get<Mesh>(firstMeshId);
		const auto second = restored.Get<Mesh>(secondMeshId);
		ASSERT_TRUE(first && second);
		ASSERT_TRUE(first->o_mesh);
		EXPECT_EQ(first->o_mesh.get(), second->o_mesh.get());
		EXPECT_EQ(first->o_mesh.shared(), restored.Get<MeshData>(geometryId));
		EXPECT_EQ(first->o_mesh->GetVertexCount(), 3u);
		ASSERT_TRUE(restored.Get<UID>(plainId));
	};
	ResourceManager reopened;
	{
		project::Project reader;
		reader.Register<project::ResourceComponent>(reopened);
		reader.Load(copy.string());
	}
	checkRestored(reopened);

	Json saved;
	std::ifstream(copy) >> saved;
	const auto& component = saved.at("project");
	std::set<int> keys;
	for (const auto& entry : component.at("m_resources").at("resources_"))
	{
		EXPECT_EQ(entry.size(), 3u);
		EXPECT_TRUE(entry.at("type").is_string());
		EXPECT_EQ(entry.at("data").dump().find("\"o_id\""), std::string::npos);
		EXPECT_TRUE(keys.insert(entry.at("key").get<int>()).second);
	}
}

TEST(ProjectRoundtrip, RecursiveLookupRejectsSelfReferenceAndKeepsIndependentResource)
{
	ResourceManager source;
	const auto mesh = source.Load<Mesh>();
	const auto camera = source.Load<Camera>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	const int meshId = mesh->GetObjectID();
	for (auto& record : document.at("m_resources").at("resources_"))
		if (record.at("key") == meshId)
			record.at("data")["o_meshDataId"] = meshId;

	ResourceManager restored;
	project::ResourceComponent(restored).LoadJson(document.dump());
	ASSERT_TRUE(restored.Contains(meshId));
	EXPECT_FALSE(restored.Get<Mesh>(meshId));
	EXPECT_FALSE(restored.Contains(meshId));
	EXPECT_TRUE(restored.Get<Camera>(camera->GetObjectID()));
}

TEST(ProjectRoundtrip, DuplicateUidsDoNotMaterializeTheirFirstRecord)
{
	ResourceManager source;
	const auto camera = source.Load<Camera>();
	camera->cam_pers = 37.0f;
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	auto& records = document.at("m_resources").at("resources_");
	auto duplicate = records.front();
	duplicate["data"]["cam_pers"] = 54.0f;
	records.push_back(std::move(duplicate));

	ResourceManager restored;
	project::ResourceComponent(restored).LoadJson(document.dump());
	const auto id = camera->GetObjectID();
	EXPECT_EQ(restored.Size(), 1u);
	EXPECT_TRUE(restored.Contains(id));
	EXPECT_FALSE(restored.GetLive(id));
	const auto loaded = restored.Get<Camera>(id);
	ASSERT_TRUE(loaded);
	EXPECT_FLOAT_EQ(loaded->cam_pers, 37.0f);
}

TEST(ProjectRoundtrip, ImmediateSaveOmitsCameraWithInvalidKey)
{

	ResourceManager source;
	const int broken = source.Load<Camera>()->GetObjectID();
	const int valid = source.Load<Camera>()->GetObjectID();
	std::ostringstream original;
	{
		cereal::JSONOutputArchive archive(original);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(original.str());
	for (auto& entry : document.at("m_resources").at("resources_"))
		if (entry.at("key") == broken)
			entry.at("key") = -1;

	ResourceManager pool;
	project::ResourceComponent restored(pool);
	restored.LoadJson(document.dump());
	EXPECT_FALSE(pool.Contains(broken));
	EXPECT_TRUE(pool.Contains(valid));
	EXPECT_FALSE(pool.GetLive(valid));
	std::ostringstream saved;
	{
		cereal::JSONOutputArchive archive(saved);
		ASSERT_NO_THROW(restored.Save(archive));
	}
	EXPECT_FALSE(pool.GetLive(valid));
	ResourceManager reopened;
	project::ResourceComponent(reopened).LoadJson(saved.str());
	ASSERT_EQ(reopened.Size(), 1u);
	EXPECT_TRUE(reopened.Get<Camera>(valid));
}
} // namespace
