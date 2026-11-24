#include "asset/Project.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/MeshData.hpp"
#include "core/ResourceManager.hpp"
#include "scene/Camera.hpp"
#include "scene/Mesh.hpp"
#include "shader/BuiltInRenderData.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace
{
using namespace neurus;
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

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

	const BuiltInRenderData catalog;
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
		firstMeshId = source.Load<Mesh>(geometry)->GetObjectID();
		secondMeshId = source.Load<Mesh>(geometry)->GetObjectID();
		plainId = source.Load<UID>()->GetObjectID();
		project::Project writer;
		writer.Register<project::ResourceComponent>(source, catalog);
		writer.Save(original.string());
	}
	// Older files omit the optional display name. A deferred rename must add
	// that field to the retained record when saving.
	{
		Json document;
		std::ifstream(original) >> document;
		for (auto& entry : document.at("project").at("m_resources").at("resources_"))
			if (entry.at("key") == renamedId)
				ASSERT_EQ(entry.at("value").at("ptr_wrapper").at("data").at("value0").erase("o_name"), 1u);
		std::ofstream(original) << document.dump();
	}

	ResourceManager pool;
	{
		project::Project reader;
		reader.Register<project::ResourceComponent>(pool, catalog);
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
		writer.Register<project::ResourceComponent>(pool, catalog);
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
		EXPECT_EQ(first->o_mesh, second->o_mesh);
		EXPECT_EQ(first->o_mesh, restored.Get<MeshData>(geometryId));
		EXPECT_EQ(first->o_mesh->GetVertexCount(), 3u);
		ASSERT_TRUE(restored.Get<UID>(plainId));
	};
	ResourceManager reopened;
	{
		project::Project reader;
		reader.Register<project::ResourceComponent>(reopened, catalog);
		reader.Load(copy.string());
	}
	checkRestored(reopened);

	Json saved;
	std::ifstream(copy) >> saved;
	const auto& component = saved.at("project");
	std::set<std::uint32_t> pointerIds;
	for (const auto& entry : component.at("m_resources").at("resources_"))
	{
		const auto id = entry.at("value").at("ptr_wrapper").at("id").get<std::uint32_t>();
		EXPECT_TRUE(pointerIds.insert(id).second);
	}
	std::istringstream stream(component.dump());
	cereal::JSONInputArchive archive(stream);
	ResourceManager raw;
	project::ResourceComponent(raw, catalog).Load(archive);
	checkRestored(raw);
}

TEST(ProjectRoundtrip, ImmediateSaveOmitsCameraWithMisplacedIdentity)
{
	const BuiltInRenderData catalog;
	ResourceManager source;
	const int broken = source.Load<Camera>()->GetObjectID();
	const int valid = source.Load<Camera>()->GetObjectID();
	std::ostringstream original;
	{
		cereal::JSONOutputArchive archive(original);
		project::ResourceComponent(source, catalog).Save(archive);
	}
	auto document = Json::parse(original.str());
	for (auto& entry : document.at("m_resources").at("resources_"))
		if (entry.at("key") == broken)
			entry.at("value").at("ptr_wrapper").at("data") = Json{{"o_id", broken}};

	ResourceManager pool;
	project::ResourceComponent restored(pool, catalog);
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
	project::ResourceComponent(reopened, catalog).LoadJson(saved.str());
	ASSERT_EQ(reopened.Size(), 1u);
	EXPECT_TRUE(reopened.Get<Camera>(valid));
}
} // namespace
