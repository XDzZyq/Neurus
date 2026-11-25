#include "asset/Project.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/ResourceManager.hpp"
#include "scene/Camera.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/Sprite.hpp"
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
#include <stdexcept>
#include <string>

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

TEST(ProjectRoundtrip, DuplicateUidsFailTheLoad)
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
	try
	{
		project::ResourceComponent(restored).LoadJson(document.dump());
		FAIL() << "a duplicate UID must fail the load";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("duplicate resource UID"), std::string::npos)
			<< error.what();
	}
}

TEST(ProjectRoundtrip, InvalidKeyFailsTheLoad)
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
	EXPECT_THROW(restored.LoadJson(document.dump()), std::runtime_error);
	EXPECT_FALSE(pool.Contains(valid));
}

TEST(ProjectRoundtrip, LegacyKeyValueRecordFailsTheLoad)
{
	ResourceManager source;
	source.Load<Camera>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	auto& records = document.at("m_resources").at("resources_");
	Json legacy = Json::object();
	legacy["key"] = records.front().at("key");
	legacy["value"] = Json::object();
	records.front() = legacy;

	ResourceManager restored;
	try
	{
		project::ResourceComponent(restored).LoadJson(document.dump());
		FAIL() << "a legacy {key, value} record must fail the load";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("legacy"), std::string::npos) << error.what();
	}
}

TEST(ProjectRoundtrip, UnknownResourceTypeFailsTheLoad)
{
	ResourceManager source;
	source.Load<Camera>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	for (auto& record : document.at("m_resources").at("resources_"))
		record.at("type") = "neurus::NoSuchType";

	ResourceManager restored;
	try
	{
		project::ResourceComponent(restored).LoadJson(document.dump());
		FAIL() << "an unknown pooled type must fail the load";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("unknown resource type"), std::string::npos)
			<< error.what();
	}
}

TEST(ProjectRoundtrip, RecordWithoutSavedNameRegistersItsDefaultName)
{
	ResourceManager source;
	const auto firstMesh = source.Load<Mesh>();
	const auto camera = source.Load<Camera>();
	const auto light = source.Load<Light>();
	const auto sprite = source.Load<Sprite>();
	const auto secondMesh = source.Load<Mesh>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	for (auto& record : document.at("m_resources").at("resources_"))
		record.at("data").at("value0").erase("o_name");

	ResourceManager restored;
	project::ResourceComponent(restored).LoadJson(document.dump());

	// Defaults claim names in ascending UID order without materializing anything.
	const int firstId = firstMesh->GetObjectID();
	const int secondId = secondMesh->GetObjectID();
	EXPECT_EQ(restored.RegisteredName(firstId), "Mesh");
	EXPECT_EQ(restored.RegisteredName(secondId), "Mesh.001");
	EXPECT_EQ(restored.RegisteredName(camera->GetObjectID()), "Camera");
	EXPECT_EQ(restored.RegisteredName(light->GetObjectID()), "Light");
	EXPECT_EQ(restored.RegisteredName(sprite->GetObjectID()), "Object");
	for (const int id : { firstId, secondId, camera->GetObjectID(), light->GetObjectID(),
		     sprite->GetObjectID() })
		EXPECT_FALSE(restored.GetLive(id)) << id;

	const auto resolved = restored.GetByName<Mesh>("Mesh");
	ASSERT_TRUE(resolved);
	EXPECT_EQ(resolved->GetObjectID(), firstId);
	EXPECT_EQ(resolved->o_name, restored.RegisteredName(firstId));
	EXPECT_EQ(resolved->o_name, "Mesh");
}

TEST(ProjectRoundtrip, ResavingNamelessDeferredRecordWritesItsDefaultName)
{
	ResourceManager source;
	const auto mesh = source.Load<Mesh>();
	source.Load<Camera>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	for (auto& record : document.at("m_resources").at("resources_"))
		record.at("data").at("value0").erase("o_name");

	ResourceManager restored;
	project::ResourceComponent(restored).LoadJson(document.dump());

	// Re-save without materializing: the deferred record must carry the default.
	std::ostringstream resaved;
	{
		cereal::JSONOutputArchive archive(resaved);
		project::ResourceComponent(restored).Save(archive);
	}
	const Json roundTrip = Json::parse(resaved.str());
	const int meshId = mesh->GetObjectID();
	for (const auto& record : roundTrip.at("m_resources").at("resources_"))
		if (record.at("key") == meshId)
		{
			EXPECT_EQ(record.at("data").at("value0").at("o_name").get<std::string>(), "Mesh");
			return;
		}
	FAIL() << "the mesh record must survive the resave";
}

/**
 * @test Save persists the allocator, so a key removed before the save is never
 *       handed out again after the document is reopened into a fresh manager.
 */
TEST(ProjectRoundtrip, SavedNextUidPreventsReuseOfRemovedKey)
{
	ResourceManager source;
	const int keptId = source.Load<Camera>()->GetObjectID();
	const int removedId = source.Load<Camera>()->GetObjectID();
	EXPECT_LT(keptId, removedId);
	ASSERT_TRUE(source.Remove(removedId));

	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	const auto document = Json::parse(output.str());
	EXPECT_EQ(document.at("m_resources").at("next_uid").get<int>(), removedId + 1);

	ResourceManager reopened;
	project::ResourceComponent(reopened).LoadJson(document.dump());
	EXPECT_GT(reopened.Load<Camera>()->GetObjectID(), removedId);
}

/**
 * @test A record rejected on first load disappears from the pool, but the
 *       persisted allocator still excludes its key after a resave and reopen.
 */
TEST(ProjectRoundtrip, RejectedRecordKeyIsNotReusedAfterResave)
{
	ResourceManager source;
	const int cameraId = source.Load<Camera>()->GetObjectID();
	// A never-compiled shader is unusable, so Get rejects its record.
	const int brokenId = source.Load<RenderShader>("Broken")->GetObjectID();
	EXPECT_LT(cameraId, brokenId);

	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}

	ResourceManager pool;
	project::ResourceComponent(pool).LoadJson(output.str());
	EXPECT_FALSE(pool.Get<RenderShader>(brokenId))
		<< "an uncompiled shader must be rejected on first load";
	EXPECT_FALSE(pool.Contains(brokenId));

	std::ostringstream resaved;
	{
		cereal::JSONOutputArchive archive(resaved);
		project::ResourceComponent(pool).Save(archive);
	}
	const auto document = Json::parse(resaved.str());
	EXPECT_GT(document.at("m_resources").at("next_uid").get<int>(), brokenId);

	ResourceManager reopened;
	project::ResourceComponent(reopened).LoadJson(document.dump());
	EXPECT_GT(reopened.Load<Camera>()->GetObjectID(), brokenId);
}

/**
 * @test A pool without next_uid fails the open instead of falling back to one.
 */
TEST(ProjectRoundtrip, MissingNextUidFailsTheLoad)
{
	ResourceManager source;
	source.Load<Camera>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	document.at("m_resources").erase("next_uid");

	ResourceManager pool;
	const int kept = pool.Load<Camera>()->GetObjectID();
	try
	{
		project::ResourceComponent(pool).LoadJson(document.dump());
		FAIL() << "a pool without next_uid must fail the load";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("next_uid"), std::string::npos) << error.what();
	}
	EXPECT_TRUE(pool.Contains(kept));
}

/**
 * @test A next_uid that does not exceed the highest saved key fails the open,
 *       so reopening can never reuse a key already present in the file.
 */
TEST(ProjectRoundtrip, NextUidBelowAKeyFailsTheLoad)
{
	ResourceManager source;
	source.Load<Camera>();
	source.Load<Camera>();
	std::ostringstream output;
	{
		cereal::JSONOutputArchive archive(output);
		project::ResourceComponent(source).Save(archive);
	}
	auto document = Json::parse(output.str());
	int maxKey = 0;
	for (const auto& record : document.at("m_resources").at("resources_"))
		maxKey = std::max(maxKey, record.at("key").get<int>());
	document.at("m_resources")["next_uid"] = maxKey;

	ResourceManager pool;
	const int kept = pool.Load<Camera>()->GetObjectID();
	try
	{
		project::ResourceComponent(pool).LoadJson(document.dump());
		FAIL() << "next_uid equal to the highest key must fail the load";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("next_uid"), std::string::npos) << error.what();
	}
	EXPECT_TRUE(pool.Contains(kept));
}
} // namespace
