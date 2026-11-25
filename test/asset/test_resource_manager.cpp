/**
 * @file test_resource_manager.cpp
 * @brief Tests for the asset ResourceManager (UID object pool + factory).
 *
 * Covers:
 * - Load<T> constructs + registers + returns a typed shared_ptr
 * - Load<T> auto-registers a public o_name (suffix on collision, renameable)
 *   and registers nothing for a type without one
 * - Duplicate registration throws (safety net)
 * - Get<T> type-checked lookup (wrong type -> nullptr)
 * - Contains / Remove / Clear / Size
 * - Name registry: per-type uniqueness and suffixing, typed SetName/GetByName,
 *   rename-to-own-name no-op, rename releasing the old forward entry, name
 *   release on Remove/Clear, and a load into a non-empty pool dropping old names
 * - Per-type index: ForEach<T> visits only its exact dynamic type, while
 *   ForEachUID stays complete even with a base-typed instance pooled
 * - Each manager owns its allocator, so managers allocate independently and
 *   Clear keeps the counter monotonic
 * - Polymorphic save/load round-trip (Camera + Mesh + Light + MeshData via
 *   cereal JSON archives), including UID preservation and post-load counter
 *   bump (new objects never collide with restored IDs).
 *
 * Pure CPU -- no GPU required.
 */

#include <gtest/gtest.h>

#include <cereal/archives/json.hpp>
#include <algorithm>
#include <sstream>
#include <typeindex>
#include <utility>
#include <vector>

#include "asset/ResourceManager.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "scene/Camera.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "asset/data/MeshData.hpp"
#include "shader/RenderShader.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/shader/ShaderUnit.hpp"

// Force-link the polymorphic registration TUs (static libs) so the pool's
// UID-derived types are registered for the JSON round-trip below.
#include "scene/registrations/TypeRegistration.hpp"
#include "asset/registrations/DataRegistration.hpp"
#include "shader/registrations/ShaderRegistration.hpp"

using namespace neurus;

// -----------------------------------------------------------------------
// Test-local resource types (name registry / type index)
// -----------------------------------------------------------------------

namespace
{

/**
 * @brief Minimal UID resource used to exercise the name and type indexes.
 */
struct TestResource : neurus::UID
{
};

/**
 * @brief Second concrete type, distinct from TestResource.
 */
struct OtherTestResource : neurus::UID
{
};

/**
 * @brief UID resource carrying a display name, like a scene object does.
 *
 * Named differently from the real scene types so the assertions cannot lean on
 * their constructor defaults.
 */
struct NamedResource : neurus::UID
{
	std::string o_name = "Named";
};

} // namespace

// -----------------------------------------------------------------------
// Load / Register semantics
// -----------------------------------------------------------------------

/**
 * @test Load<T> auto-registers a type's o_name and mirrors the unique result,
 *       so no creation path can forget the name.
 */
TEST(ResourceManagerTest, LoadAutoRegistersOname)
{
	ResourceManager pool;

	pool.Load<TestResource>();
	EXPECT_EQ(pool.GetByName<TestResource>("Object"), nullptr);
	EXPECT_EQ(pool.GetByName<TestResource>("Named"), nullptr);
	auto first = pool.Load<NamedResource>();
	EXPECT_EQ(first->o_name, "Named") << "the constructor name is mirrored back";
	EXPECT_EQ(pool.GetByName<NamedResource>("Named"), first);

	// The registry owns uniqueness: the second load is suffixed.
	auto second = pool.Load<NamedResource>();
	EXPECT_EQ(second->o_name, "Named.001");
	EXPECT_EQ(pool.GetByName<NamedResource>("Named.001"), second);
}

/**
 * @test A load-time registration is an ordinary one: renaming it frees the
 *       constructor name for the next resource. EditorViewport::EnsureCamera
 *       relies on exactly this when it renames the viewport camera.
 */
TEST(ResourceManagerTest, LoadRegistrationIsRenameable)
{
	ResourceManager pool;
	auto cam = pool.Load<Camera>();
	EXPECT_THROW(pool.Register(cam), std::runtime_error);
	EXPECT_EQ(cam->o_name, "Camera");
	EXPECT_EQ(pool.GetByName<Camera>("Camera"), cam);

	cam->o_name = pool.SetName(cam->GetObjectID(), "ViewportCamera");
	EXPECT_EQ(cam->o_name, "ViewportCamera");
	EXPECT_EQ(pool.SetName(cam->GetObjectID(), "ViewportCamera"), "ViewportCamera");
	EXPECT_EQ(pool.GetByName<Camera>("Camera"), nullptr);
	EXPECT_EQ(pool.GetByName<Camera>("ViewportCamera"), cam);

	auto docCam = pool.Load<Camera>();
	EXPECT_EQ(docCam->o_name, "Camera") << "the freed name must be reusable";
}

/**
 * @test Get<T> with the wrong type returns nullptr; base types resolve.
 */
TEST(ResourceManagerTest, GetTypeChecked)
{
	ResourceManager pool;
	auto cam = pool.Load<Camera>();

	EXPECT_NE(pool.Get<Camera>(cam->GetObjectID()), nullptr);
	EXPECT_NE(pool.Get<ObjectID>(cam->GetObjectID()), nullptr);
	EXPECT_NE(pool.Get<UID>(cam->GetObjectID()), nullptr);
	EXPECT_EQ(pool.Get<Mesh>(cam->GetObjectID()), nullptr); // wrong type
	EXPECT_EQ(pool.Get<Camera>(123456), nullptr);           // missing ID
}

/**
 * @test Contains / Remove / Clear / Size.
 */
TEST(ResourceManagerTest, RemoveClearSize)
{
	ResourceManager pool;
	auto cam = pool.Load<Camera>();
	auto light = pool.Load<Light>(POINTLIGHT, 5.0f, glm::vec3(1.0f));
	EXPECT_EQ(pool.Size(), 2u);

	const int cameraId = cam->GetObjectID();
	const int lightId = light->GetObjectID();
	EXPECT_TRUE(pool.Remove(cameraId));
	EXPECT_EQ(pool.Size(), 1u);
	EXPECT_FALSE(pool.Contains(cameraId));
	EXPECT_FALSE(pool.Remove(cameraId)); // already gone

	pool.Clear();
	EXPECT_EQ(pool.Size(), 0u);
	EXPECT_FALSE(pool.Contains(lightId));
	EXPECT_EQ(cam->GetObjectID(), -1);
	EXPECT_EQ(light->GetObjectID(), -1);
}

TEST(ResourceManagerTest, MembershipBelongsExclusivelyToItsPool)
{
	auto resource = std::make_shared<TestResource>();
	EXPECT_EQ(resource->GetObjectID(), -1);
	ResourceManager other;
	{
		ResourceManager owner;
		const int id = owner.Register(resource);
		EXPECT_EQ(resource->GetObjectID(), id);
		EXPECT_THROW(other.Register(resource), std::runtime_error);
		EXPECT_EQ(owner.Get(id), resource);
	}
	EXPECT_EQ(resource->GetObjectID(), -1);
	const int newId = other.Register(resource);
	EXPECT_EQ(resource->GetObjectID(), newId);
}

TEST(ResourceManagerTest, RejectedDeferredObjectLosesItsMembership)
{
	ResourceManager pool;
	auto object = std::make_shared<TestResource>();
	pool.RegisterDeferred(0, typeid(TestResource), [&] { return object; },
		std::make_shared<const std::string>("record"), [](UID& candidate) {
			EXPECT_EQ(candidate.GetObjectID(), 0);
			return false;
		});
	EXPECT_FALSE(pool.Get(0));
	EXPECT_FALSE(pool.Contains(0));
	EXPECT_EQ(object->GetObjectID(), -1);
}

TEST(ResourceManagerTest, DeferredMetadataAndNamedLookup)
{
	ResourceManager pool;
	auto resource = std::make_shared<TestResource>();
	const int id = 42;
	int loads = 0;
	pool.RegisterDeferred(id, typeid(TestResource), [&] { ++loads; return resource; });

	EXPECT_TRUE(pool.Contains(id));
	EXPECT_EQ(pool.Size(), 1u);
	EXPECT_EQ(pool.SetName(id, "Original"), "Original");
	EXPECT_EQ(pool.SetName(id, "Renamed"), "Renamed");
	EXPECT_EQ(pool.GetByName<TestResource>("Original"), nullptr);
	EXPECT_EQ(pool.GetByName<OtherTestResource>("Renamed"), nullptr);
	EXPECT_EQ(pool.Get(-1), nullptr);
	EXPECT_EQ(loads, 0);

	const IResourceLookup& lookup = pool;
	EXPECT_EQ(lookup.GetByName<TestResource>("Renamed"), resource);
	EXPECT_EQ(lookup.Get<TestResource>(id), resource);
	EXPECT_EQ(lookup.Get<OtherTestResource>(id), nullptr);
	EXPECT_EQ(pool.Size(), 1u);
	EXPECT_EQ(loads, 1);
}

TEST(ResourceManagerTest, DeferredEnumerationLoadsMatchingResourcesOnce)
{
	ResourceManager pool;
	auto first = std::make_shared<TestResource>();
	auto second = std::make_shared<OtherTestResource>();
	int firstLoads = 0;
	int secondLoads = 0;
	pool.RegisterDeferred(10, typeid(TestResource), [&] { ++firstLoads; return first; });
	pool.RegisterDeferred(20, typeid(OtherTestResource), [&] { ++secondLoads; return second; });

	int visits = 0;
	pool.ForEach<TestResource>([&](const std::shared_ptr<TestResource>& obj) {
		EXPECT_EQ(obj, first);
		++visits;
	});
	EXPECT_EQ(visits, 1);
	EXPECT_EQ(firstLoads, 1);
	EXPECT_EQ(secondLoads, 0);

	std::vector<std::shared_ptr<UID>> seen;
	pool.ForEachUID([&](const std::shared_ptr<UID>& obj) { seen.push_back(obj); });
	EXPECT_EQ(seen.size(), 2u);
	EXPECT_EQ(std::count(seen.begin(), seen.end(), first), 1);
	EXPECT_EQ(std::count(seen.begin(), seen.end(), second), 1);
	EXPECT_EQ(firstLoads, 1);
	EXPECT_EQ(secondLoads, 1);
}

TEST(ResourceManagerTest, DeferredIdsAreReservedUntilRemoveOrClear)
{
	ResourceManager pool;
	const int futureId = pool.NextId() + 20;
	int loads = 0;
	auto loader = [&]() -> std::shared_ptr<UID> { ++loads; return nullptr; };
	pool.RegisterDeferred(futureId, typeid(TestResource), loader);
	EXPECT_GT(pool.Load<TestResource>()->GetObjectID(), futureId);
	EXPECT_THROW(pool.RegisterDeferred(futureId, typeid(TestResource), loader), std::exception);
	EXPECT_EQ(pool.SetName(futureId, "Spare"), "Spare");
	EXPECT_TRUE(pool.Remove(futureId));
	EXPECT_EQ(pool.GetByName<TestResource>("Spare"), nullptr);
	auto live = pool.Load<TestResource>();
	EXPECT_GT(live->GetObjectID(), futureId);
	EXPECT_THROW(pool.RegisterDeferred(live->GetObjectID(), typeid(TestResource), loader), std::exception);
	pool.Clear();
	EXPECT_EQ(pool.Size(), 0u);
	EXPECT_EQ(pool.Get(futureId), nullptr);
	EXPECT_EQ(loads, 0);
}

/**
 * @test Each manager owns its allocator, so two fresh managers both hand out 1.
 */
TEST(ResourceManagerTest, ManagersAllocateIndependently)
{
	ResourceManager first;
	ResourceManager second;
	EXPECT_EQ(first.Load<TestResource>()->GetObjectID(), 1);
	EXPECT_EQ(second.Load<OtherTestResource>()->GetObjectID(), 1);
	EXPECT_EQ(first.NextId(), 2);
	EXPECT_EQ(second.NextId(), 2);
}

/**
 * @test Clear drops the objects but keeps the allocator, so keys stay unique for
 *       this manager's lifetime.
 */
TEST(ResourceManagerTest, ClearKeepsTheAllocatorMonotonic)
{
	ResourceManager pool;
	const int firstId = pool.Load<TestResource>()->GetObjectID();
	pool.Clear();
	EXPECT_EQ(pool.Size(), 0u);
	EXPECT_GT(pool.NextId(), firstId);
	const int freshId = pool.Load<TestResource>()->GetObjectID();
	EXPECT_GT(freshId, firstId);
	EXPECT_EQ(pool.NextId(), freshId + 1);
}

TEST(ResourceManagerTest, RejectsIncompleteOrMismatchedDeferredObjects)
{
	ResourceManager pool;
	auto expected = std::make_shared<TestResource>();
	auto other = pool.Load<TestResource>();
	auto wrongType = std::make_shared<OtherTestResource>();
	std::shared_ptr<UID> result;
	const int id = pool.NextId();
	int attempts = 0;
	pool.RegisterDeferred(id, typeid(TestResource), [&] { ++attempts; return result; });
	EXPECT_THROW(pool.Get(id), std::exception);
	result = other;
	EXPECT_THROW(pool.Get(id), std::exception);
	result = expected;
	EXPECT_EQ(pool.Get(id), expected);
	EXPECT_EQ(pool.Get(id), expected);
	EXPECT_EQ(attempts, 3);

	pool.RegisterDeferred(id + 1, typeid(TestResource), [&] { return wrongType; });
	EXPECT_THROW(pool.Get(id + 1), std::exception);
}

TEST(ResourceManagerTest, DeferredCycleUnwindsAndDependenciesCanBeRetried)
{
	ResourceManager pool;
	auto parent = std::make_shared<TestResource>();
	auto child = std::make_shared<OtherTestResource>();
	auto independent = pool.Load<TestResource>();
	int parentLoads = 0;
	int childLoads = 0;
	bool cyclic = true;
	const int parentId = pool.NextId();
	const int childId = parentId + 1;
	pool.RegisterDeferred(parentId, typeid(TestResource), [&] {
		++parentLoads;
		EXPECT_EQ(pool.Get(childId), child);
		return parent;
	});
	pool.RegisterDeferred(childId, typeid(OtherTestResource), [&] {
		++childLoads;
		if (cyclic)
			pool.Get(parentId);
		return child;
	});
	EXPECT_THROW(pool.Get(parentId), std::exception);
	EXPECT_EQ(pool.Get(independent->GetObjectID()), independent);
	cyclic = false;
	EXPECT_EQ(pool.Get(parentId), parent);
	EXPECT_EQ(pool.Get(childId), child);
	EXPECT_EQ(parentLoads, 2);
	EXPECT_EQ(childLoads, 2);
}

TEST(ResourceManagerTest, ProjectArchiveIncludesDeferredEntriesAndReplacesOldRegistry)
{
	ResourceManager source;
	auto camera = std::make_shared<Camera>();
	camera->cam_pers = 42.0f;
	const int cameraId = 257;
	source.RegisterDeferred(cameraId, typeid(Camera), [camera] { return camera; });
	std::stringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		project::ResourceComponent(source).Save(archive);
	}

	ResourceManager target;
	auto stale = std::make_shared<Camera>();
	int staleLoads = 0;
	target.RegisterDeferred(365, typeid(Camera), [&] { ++staleLoads; return stale; });
	target.SetName(365, "Previous");
	project::ResourceComponent(target).LoadJson(stream.str());
	EXPECT_FALSE(target.GetLive(cameraId));
	EXPECT_EQ(target.Size(), 1u);
	EXPECT_FALSE(target.Contains(365));
	EXPECT_EQ(target.GetByName<Camera>("Previous"), nullptr);
	EXPECT_EQ(staleLoads, 0);
	auto restored = target.Get<Camera>(camera->GetObjectID());
	ASSERT_NE(restored, nullptr);
	EXPECT_FLOAT_EQ(restored->cam_pers, 42.0f);
}

// -----------------------------------------------------------------------
// Polymorphic save/load round-trip
// -----------------------------------------------------------------------

/**
 * @test A pool containing Camera + MeshData + Mesh + Light round-trips
 *       through cereal JSON archives: type, IDs, and per-type fields survive.
 */
TEST(ResourceManagerTest, PoolSaveLoadRoundtrip)
{
	ResourceManager pool;

	auto cam = pool.Load<Camera>();
	cam->o_name = "Cam1";
	cam->cam_pers = 42.0f;

	auto meshData = pool.Load<MeshData>("obj/sphere.obj");
	auto mesh = pool.Load<Mesh>(meshData);
	mesh->o_name = "Mesh1";
	mesh->using_shadow = false;

	auto light = pool.Load<Light>(POINTLIGHT, 7.0f, glm::vec3(1.0f, 0.5f, 0.2f));
	light->o_name = "Light1";

	EXPECT_EQ(pool.Size(), 4u);

	// --- Save ---
	std::stringstream ss;
	{
		cereal::JSONOutputArchive ar(ss);
		project::ResourceComponent(pool).Save(ar);
	}

	// --- Load ---
	ResourceManager loaded;
	project::ResourceComponent(loaded).LoadJson(ss.str());

	EXPECT_EQ(loaded.Size(), 4u);

	// Type + identity preserved, and new IDs are allocated past restored ones.
	auto cam2 = loaded.Get<Camera>(cam->GetObjectID());
	ASSERT_NE(cam2, nullptr);
	EXPECT_EQ(cam2->o_name, "Cam1");
	EXPECT_FLOAT_EQ(cam2->cam_pers, 42.0f);

	auto mesh2 = loaded.Get<Mesh>(mesh->GetObjectID());
	ASSERT_NE(mesh2, nullptr);
	EXPECT_EQ(mesh2->o_name, "Mesh1");
	EXPECT_FALSE(mesh2->using_shadow);
	EXPECT_EQ(mesh2->o_mesh.shared(), loaded.Get<MeshData>(meshData->GetObjectID()));
	EXPECT_EQ(mesh2->o_mesh.id(), meshData->GetObjectID());

	auto light2 = loaded.Get<Light>(light->GetObjectID());
	ASSERT_NE(light2, nullptr);
	EXPECT_EQ(light2->o_name, "Light1");
	EXPECT_EQ(light2->light_type, POINTLIGHT);
	EXPECT_FLOAT_EQ(light2->light_power, 7.0f);
	EXPECT_EQ(light2->light_color, glm::vec3(1.0f, 0.5f, 0.2f));

	// Wrong-type lookups stay null after the round-trip.
	EXPECT_EQ(loaded.Get<Mesh>(cam->GetObjectID()), nullptr);

	// The manager reserves restored record keys: fresh allocations never
	// collide with the loaded ones.
	auto fresh = loaded.Load<Camera>();
	EXPECT_GT(fresh->GetObjectID(), cam->GetObjectID());
	EXPECT_NE(fresh->GetObjectID(), cam->GetObjectID());
	EXPECT_NE(fresh->GetObjectID(), mesh->GetObjectID());
}

/**
 * @test A pooled RenderShader (the per-mesh "created shader") round-trips
 *       through the pool. Serialization stores only the source paths, but
 *       RenderShader::serialize(load) re-parses content from disk AND
 *       recompiles to SPIR-V (CompileToSpv), so the loaded shader is ready for
 *       GeometryPass's per-mesh pipeline without any post-load recompile step.
 *
 *       Regression: "[GeometryPass] Per-mesh pipeline creation failed" after
 *       create shader → save project → reopen app (SPIR-V was lost on load).
 */
TEST(ResourceManagerTest, PooledShaderRecompilesAfterLoad)
{
	ResourceManager pool;

	// Mirrors Editor::OnCreateShader: pooled RenderShader + parse + compile.
	auto shader = pool.Load<RenderShader>(
		"MeshShader_10", "res/shaders/render/gbuffer.vert.glsl", "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_NE(shader, nullptr);
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	ASSERT_FALSE(shader->GetStage(ShaderType::VERTEX).spv.empty());
	ASSERT_FALSE(shader->GetStage(ShaderType::FRAGMENT).spv.empty());

	// --- Save (the "save and quit" step) ---
	std::stringstream ss;
	{
		cereal::JSONOutputArchive ar(ss);
		project::ResourceComponent(pool).Save(ar);
	}

	// --- Load (the "reopen the app" step) ---
	ResourceManager loaded;
	project::ResourceComponent(loaded).LoadJson(ss.str());

	// The load lifecycle restores derived state: serialize(load) re-parses the
	// stages and recompiles to SPIR-V, so GeometryPass can build the pipeline
	// on the first frame (no per-frame "pipeline creation failed" fallback).
	auto shader2 = loaded.Get<RenderShader>(shader->GetObjectID());
	ASSERT_NE(shader2, nullptr);
	EXPECT_FALSE(shader2->GetStage(ShaderType::VERTEX).spv.empty());
	EXPECT_FALSE(shader2->GetStage(ShaderType::FRAGMENT).spv.empty());
	EXPECT_GT(shader2->GetVersion(), 0);
}

// -----------------------------------------------------------------------
// Name registry (per concrete type)
// -----------------------------------------------------------------------

/**
 * @test Names are unique per concrete type: both types may be "Ground", while
 *       a second resource of the same type gains the ".001" suffix.
 */
TEST(ResourceNameRegistry, SetAndGetByNamePerType)
{
	neurus::ResourceManager rm;
	auto a = rm.Load<TestResource>();
	auto b = rm.Load<TestResource>();
	auto c = rm.Load<OtherTestResource>();

	EXPECT_EQ(rm.SetName(a->GetObjectID(), "Ground"), "Ground");
	EXPECT_EQ(rm.SetName(b->GetObjectID(), "Ground"), "Ground.001");
	EXPECT_EQ(rm.SetName<OtherTestResource>(c->GetObjectID(), "Ground"), "Ground");

	EXPECT_EQ(rm.GetByName<TestResource>("Ground"), a);          // first keeps the bare name
	EXPECT_EQ(rm.GetByName<TestResource>("Ground.001"), b);      // second is suffixed
	EXPECT_EQ(rm.GetByName<OtherTestResource>("Ground"), c);     // own namespace
	EXPECT_EQ(rm.GetByName<TestResource>("Missing"), nullptr);
	EXPECT_EQ(rm.GetByName<TestResource>("NoSuchType"), nullptr);
}

TEST(ResourceNameRegistry, TypedSetterRejectsWrongType)
{
	neurus::ResourceManager rm;
	auto c = rm.Load<OtherTestResource>();
	EXPECT_EQ(rm.SetName<TestResource>(c->GetObjectID(), "Ground"), std::string{});
	EXPECT_EQ(rm.GetByName<OtherTestResource>("Ground"), nullptr);
	EXPECT_TRUE(rm.SetName(123456, "Ground").empty());
}

/**
 * @test Remove frees the name; Clear drops the registry and type index too.
 */
TEST(ResourceNameRegistry, RemoveFreesNameAndClearEmptiesIndex)
{
	neurus::ResourceManager rm;
	auto a = rm.Load<TestResource>();
	ASSERT_EQ(rm.SetName(a->GetObjectID(), "Ground"), "Ground");
	ASSERT_TRUE(rm.Remove(a->GetObjectID()));
	EXPECT_EQ(rm.GetByName<TestResource>("Ground"), nullptr);
	int remaining = 0;
	rm.ForEach<TestResource>([&](const std::shared_ptr<TestResource>&) { ++remaining; });
	EXPECT_EQ(remaining, 0);

	auto b = rm.Load<TestResource>();
	ASSERT_EQ(rm.SetName(b->GetObjectID(), "Ground"), "Ground");
	rm.Clear();
	EXPECT_EQ(rm.GetByName<TestResource>("Ground"), nullptr);
	EXPECT_EQ(rm.Size(), 0u);
}

/**
 * @test An empty requested name becomes "Object" (suffixed on collision).
 */
TEST(ResourceNameRegistry, EmptyNameBecomesObject)
{
	neurus::ResourceManager rm;
	auto a = rm.Load<TestResource>();
	auto b = rm.Load<TestResource>();
	EXPECT_EQ(rm.SetName(a->GetObjectID(), ""), "Object");
	EXPECT_EQ(rm.SetName(b->GetObjectID(), ""), "Object.001");
}

// -----------------------------------------------------------------------
// Per-type index
// -----------------------------------------------------------------------

/**
 * @test ForEach<T> visits only resources of T, served from the type index.
 */
TEST(ResourceTypeIndex, ForEachTypedVisitsOnlyThatType)
{
	neurus::ResourceManager rm;
	auto a = rm.Load<TestResource>();
	rm.Load<OtherTestResource>();
	std::vector<int> seen;
	rm.ForEach<TestResource>([&](const std::shared_ptr<TestResource>& r)
	{
		seen.push_back(r->GetObjectID());
	});
	ASSERT_EQ(seen.size(), 1u);
	EXPECT_EQ(seen.front(), a->GetObjectID());
}

/**
 * @test A pooled base instance must not hide derived objects from base
 *       enumeration. The type index keys exact dynamic types, so ForEachUID is
 *       the complete walk; ForEach<ObjectID> is pinned as exact-type only.
 *
 *       This is the invariant the load seeder and the script collections rely
 *       on: once a bare ObjectID is pooled, ForEach<ObjectID> would see only it.
 */
TEST(ResourceTypeIndex, BaseInstanceDoesNotHideDerivedObjectsFromForEachUID)
{
	neurus::ResourceManager rm;
	auto plain = rm.Load<neurus::ObjectID>();
	auto cam = rm.Load<neurus::Camera>();
	auto meshData = rm.Load<neurus::MeshData>("");
	auto mesh = rm.Load<neurus::Mesh>(meshData);

	const neurus::IResourceLookup& lookup = rm;
	std::vector<int> seen;
	lookup.ForEachUID([&](const std::shared_ptr<neurus::UID>& obj)
	{
		seen.push_back(obj->GetObjectID());
	});
	ASSERT_EQ(seen.size(), 4u) << "ForEachUID must be the complete walk";
	for (const int id : {plain->GetObjectID(), cam->GetObjectID(), mesh->GetObjectID()})
	{
		EXPECT_TRUE(std::find(seen.begin(), seen.end(), id) != seen.end())
			<< "ForEachUID missed uid " << id;
	}

	// The trap this guards: the plain ObjectID owns the typeid(ObjectID) index
	// entry, so an exact-type walk sees only it - cameras and meshes vanish.
	int objects = 0;
	rm.ForEach<neurus::ObjectID>([&](const std::shared_ptr<neurus::ObjectID>&) { ++objects; });
	EXPECT_EQ(objects, 1) << "ForEach<T> is exact-dynamic-type, not a base walk";

	ASSERT_TRUE(rm.Remove(plain->GetObjectID()));
	objects = 0;
	rm.ForEach<neurus::ObjectID>([&](const auto&) { ++objects; });
	EXPECT_EQ(objects, 2) << "removing the last exact match restores the base-type scan";
}

/**
 * @test A type with no pooled instance (here the UID base) has no index entry,
 *       so ForEachOfType falls back to scanning the whole pool. This is the
 *       mechanism, NOT an enumeration idiom: pooling a UID instance would create
 *       the entry and stop the fallback.
 */
TEST(ResourceTypeIndex, UntrackedTypeForEachFallsBackToEverything)
{
	neurus::ResourceManager rm;
	rm.Load<TestResource>();
	rm.Load<OtherTestResource>();
	int count = 0;
	rm.ForEach<neurus::UID>([&](const std::shared_ptr<neurus::UID>&) { ++count; });
	EXPECT_EQ(count, 2);
}

/**
 * @test The project reader indexes types and names before restoring objects.
 */
TEST(ResourceTypeIndex, LoadIndexesTypesAndNamesBeforeRestoringObjects)
{
	ResourceManager pool;
	auto cam = pool.Load<Camera>();
	pool.Load<Light>(POINTLIGHT, 5.0f, glm::vec3(1.0f));
	cam->o_name = pool.SetName(cam->GetObjectID(), "Cam");

	std::stringstream ss;
	{
		cereal::JSONOutputArchive ar(ss);
		project::ResourceComponent(pool).Save(ar);
	}

	ResourceManager loaded;
	project::ResourceComponent(loaded).LoadJson(ss.str());

	EXPECT_EQ(loaded.RegisteredName(cam->GetObjectID()), "Cam");
	EXPECT_FALSE(loaded.GetLive(cam->GetObjectID()));
	int cameras = 0;
	int lights = 0;
	loaded.ForEach<Camera>([&](const std::shared_ptr<Camera>&) { ++cameras; });
	loaded.ForEach<Light>([&](const std::shared_ptr<Light>&) { ++lights; });
	EXPECT_EQ(cameras, 1);
	EXPECT_EQ(lights, 1);

	const auto named = loaded.GetByName<Camera>("Cam");
	ASSERT_TRUE(named);
	EXPECT_EQ(named->GetObjectID(), cam->GetObjectID());
}

/**
 * @test A load into a NON-EMPTY pool drops the old name registry. Names are not
 *       derived state a rebuild can reproduce, so without the clear a name whose
 *       object the load replaced would keep reserving the bare spelling and force
 *       the next resource to "Stale.001".
 */
TEST(ResourceNameRegistry, LoadIntoNonEmptyPoolDropsTheOldRegistry)
{
	ResourceManager source;
	source.Load<Camera>();

	std::stringstream ss;
	{
		cereal::JSONOutputArchive ar(ss);
		project::ResourceComponent(source).Save(ar);
	}

	ResourceManager target;
	auto stale = target.Load<Camera>();
	ASSERT_EQ(target.SetName(stale->GetObjectID(), "Stale"), "Stale");

	project::ResourceComponent(target).LoadJson(ss.str());

	// The old object is gone with the replaced pool, and its name must be free.
	EXPECT_EQ(target.GetByName<Camera>("Stale"), nullptr);
	auto fresh = target.Load<Camera>();
	EXPECT_EQ(target.SetName(fresh->GetObjectID(), "Stale"), "Stale")
		<< "the previous document's name must not survive the load";
}

/**
 * @test ForEachRegistered reports every reserved key with its exact dynamic type
 *       in ascending order and never runs a deferred loader.
 */
TEST(ResourceManagerTest, ForEachRegisteredVisitsDeferredRecordsWithoutLoading)
{
	ResourceManager pool;
	const auto live = pool.Load<NamedResource>();
	const int liveId = live->GetObjectID();
	const int base = pool.NextId() + 50;
	int loads = 0;
	auto loader = [&loads] { ++loads; return std::make_shared<TestResource>(); };
	pool.RegisterDeferred(base + 20, typeid(TestResource), loader);
	pool.RegisterDeferred(base, typeid(OtherTestResource), loader);
	pool.RegisterDeferred(base + 10, typeid(TestResource), loader);

	std::vector<std::pair<int, std::type_index>> seen;
	pool.ForEachRegistered([&seen](int id, std::type_index type)
	{
		seen.emplace_back(id, type);
	});

	ASSERT_EQ(seen.size(), 4u);
	ASSERT_LT(liveId, base);
	EXPECT_EQ(seen[0].first, liveId);
	EXPECT_EQ(seen[0].second, std::type_index(typeid(NamedResource)));
	EXPECT_EQ(seen[1].first, base);
	EXPECT_EQ(seen[1].second, std::type_index(typeid(OtherTestResource)));
	EXPECT_EQ(seen[2].first, base + 10);
	EXPECT_EQ(seen[2].second, std::type_index(typeid(TestResource)));
	EXPECT_EQ(seen[3].first, base + 20);
	EXPECT_EQ(seen[3].second, std::type_index(typeid(TestResource)));
	EXPECT_EQ(loads, 0);
	EXPECT_FALSE(pool.GetLive(base));
}

/**
 * @test ForEachLive visits only materialized objects; a deferred record of the
 *       same type stays deferred and its loader never runs.
 */
TEST(ResourceManagerTest, ForEachLiveSkipsDeferredRecords)
{
	ResourceManager pool;
	const auto first = pool.Load<TestResource>();
	pool.Load<OtherTestResource>();
	const int deferredId = pool.NextId() + 10;
	int loads = 0;
	pool.RegisterDeferred(deferredId, typeid(TestResource),
		[&loads]
		{
			++loads;
			return std::make_shared<TestResource>();
		});

	std::vector<int> seen;
	pool.ForEachLive<TestResource>([&seen](const std::shared_ptr<TestResource>& obj)
	{
		seen.push_back(obj->GetObjectID());
	});

	ASSERT_EQ(seen.size(), 1u);
	EXPECT_EQ(seen.front(), first->GetObjectID());
	EXPECT_EQ(loads, 0);
	EXPECT_FALSE(pool.GetLive(deferredId));
}
