/**
 * @file test_project_roundtrip.cpp
 * @brief Roundtrip serialization tests for Project Save -> Load.
 *
 * Verifies that Project::Save() followed by Project::Load() preserves
 * all scene data (cameras, meshes, lights) with exact parameter matching.
 *
 * The project now stores the ResourceManager pool FIRST (real objects,
 * polymorphically), then the Scene's ID references, which SceneComponent
 * resolves against the pool after load. These tests drive the same
 * component ordering as Application::BuildProject.
 *
 * Tests create bare Scene+RenderConfig+ResourceManager objects, register
 * them with a Project serializer via ResourceComponent/SceneComponent/
 * ConfigComponent, then Save/Load through the Project.
 *
 * TDD: RED (test written first) -> GREEN (implementation verified).
 * All tests are pure CPU -- no GPU required.
 */

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <memory>
#include <sstream>

#include <cstdio>   // std::remove

#include "asset/Project.hpp"
#include "app/ApplicationCore.hpp"
#include "asset/data/ImageData.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "core/Log.hpp"
#include "editor/Editor.hpp"
#include "editor/RenderManager.hpp"
#include "render_api/RenderRequestSink.hpp"
#include "shader/ComputeShader.hpp"
#include "asset/components/SceneComponent.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/data/MeshData.hpp"
#include "core/ResourceManager.hpp"
#include "core/RenderConfig.hpp"
#include "scene/Camera.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

using namespace neurus;

// -----------------------------------------------------------------------
// RAII temporary file cleaner
// -----------------------------------------------------------------------

/**
 * @brief RAII wrapper that removes the temporary file on destruction.
 *
 * Ensures test-generated .neurus.json files are cleaned up even if
 * the test fails with an exception.
 */
struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

/**
 * @brief Creates a Project serializer with ResourceComponent + SceneComponent
 *        + ConfigComponent bound (mirrors Application::BuildProject ordering).
 *
 * @param scene     Scene object to serialize.
 * @param config    RenderConfig object to serialize.
 * @param resources ResourceManager pool (registered first).
 * @return Initialised project::Project ready for Save/Load.
 */
static project::Project MakeProject(Scene& scene, std::shared_ptr<RenderConfig>& config, ResourceManager& resources,
                                    const BuiltInRenderData& catalog)
{
	project::Project p;
	p.Register<project::ResourceComponent>(resources, catalog);
	p.Register<project::SceneComponent>(scene, resources);
	p.Register<project::ConfigComponent>(config);
	return p;
}

// -----------------------------------------------------------------------
// Roundtrip: Empty Scene
// -----------------------------------------------------------------------

/**
 * @test Save an empty project, load it back -- it stays empty. Nothing is
 *       injected to make it renderable: a camera-less scene is legal, because
 *       the viewport looks through the viewport's own camera unless the scene
 *       names an activated one.
 */
TEST(ProjectRoundtrip, EmptyScene)
{
	const neurus::BuiltInRenderData catalog;
	TempFile tmp("test_rt_empty.neurus.json");
	{
		Scene scene;
		auto config = std::make_shared<RenderConfig>();
		ResourceManager resources;
		auto p = MakeProject(scene, config, resources, catalog);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	auto loadedConfig = std::make_shared<RenderConfig>();
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources, catalog);
		p.Load(tmp.path);
	}
	EXPECT_TRUE(loadedScene.cam_list.empty());
	EXPECT_EQ(loadedScene.GetActiveCamera(), nullptr);
	EXPECT_EQ(loadedScene.ActiveCameraID(), 0);
	EXPECT_TRUE(loadedScene.mesh_list.empty());
	EXPECT_TRUE(loadedScene.light_list.empty());
	EXPECT_TRUE(loadedScene.sprite_list.empty());
	EXPECT_TRUE(loadedScene.dLine_list.empty());
	EXPECT_TRUE(loadedScene.dPoints_list.empty());
}

// -----------------------------------------------------------------------
// Roundtrip: Full Scene (camera + mesh + light)
// -----------------------------------------------------------------------

/**
 * @test Create a full scene with camera, mesh, and light. Save, load --
 *       all three objects present with correct parameters.
 */
TEST(ProjectRoundtrip, FullScene)
{
	const neurus::BuiltInRenderData catalog;
	TempFile tmp("test_rt_full.neurus.json");
	int meshDataId = 0;
	{
		Scene scene;
		auto config = std::make_shared<RenderConfig>();
		ResourceManager resources;

		// Camera
		auto camera = resources.Load<Camera>();
		camera->SetPosition(glm::vec3(0.0f, 10.0f, 3.0f));
		camera->cam_tar = glm::vec3(0.0f, 0.0f, 1.0f);
		camera->cam_pers = 45.0f;
		camera->cam_near = 0.1f;
		camera->cam_far = 100.0f;
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->GetObjectID());

		// Mesh
		auto meshData = resources.Load<MeshData>("res/obj/sphere.obj");
		meshDataId = meshData->GetObjectID();
		auto mesh = resources.Load<Mesh>(meshData);
		mesh->using_shadow = false;
		mesh->using_sdf = false;
		mesh->o_name = "TestSphere";
		mesh->SetPosition(glm::vec3(1.0f, 0.0f, 0.0f));
		scene.UseMesh(mesh);

		// Light
		auto light = resources.Load<Light>(POINTLIGHT, 20.0f, glm::vec3(0.2f, 0.5f, 1.0f));
		light->SetPosition(glm::vec3(-2.0f, 0.0f, 5.0f));
		light->SetRadius(0.1f);
		scene.UseLight(light);

		auto p = MakeProject(scene, config, resources, catalog);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	auto loadedConfig = std::make_shared<RenderConfig>();
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources, catalog);
		p.Load(tmp.path);
	}

	// Pool sizes
	ASSERT_EQ(loadedScene.cam_list.size(), 1u);
	ASSERT_EQ(loadedScene.mesh_list.size(), 1u);
	ASSERT_EQ(loadedScene.light_list.size(), 1u);

	// --- Camera validation ---
	auto* loadedCam = loadedScene.cam_list.begin()->second.get();
	ASSERT_NE(loadedCam, nullptr);
	EXPECT_EQ(loadedCam->GetPosition(), glm::vec3(0.0f, 10.0f, 3.0f));
	EXPECT_EQ(loadedCam->cam_tar, glm::vec3(0.0f, 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(loadedCam->cam_pers, 45.0f);
	EXPECT_FLOAT_EQ(loadedCam->cam_near, 0.1f);
	EXPECT_FLOAT_EQ(loadedCam->cam_far, 100.0f);
	EXPECT_EQ(loadedCam->o_type, ObjectID::GOType::GO_CAM);

	// --- Mesh validation ---
	auto* loadedMesh = loadedScene.mesh_list.begin()->second.get();
	ASSERT_NE(loadedMesh, nullptr);
	EXPECT_EQ(loadedMesh->o_meshDataId, meshDataId);
	EXPECT_NE(meshDataId, 0);
	EXPECT_FALSE(loadedMesh->using_shadow);
	EXPECT_FALSE(loadedMesh->using_sdf);
	EXPECT_EQ(loadedMesh->o_name, "TestSphere");
	ASSERT_NE(loadedMesh->o_mesh, nullptr);
	EXPECT_EQ(loadedMesh->o_mesh->GetObjectID(), meshDataId);
	EXPECT_GT(loadedMesh->o_mesh->GetVertexCount(), 0u);
	EXPECT_EQ(loadedMesh->GetPosition(), glm::vec3(1.0f, 0.0f, 0.0f));
	EXPECT_EQ(loadedMesh->o_type, ObjectID::GOType::GO_MESH);

	// --- Light validation ---
	auto* loadedLight = loadedScene.light_list.begin()->second.get();
	ASSERT_NE(loadedLight, nullptr);
	EXPECT_EQ(loadedLight->light_type, POINTLIGHT);
	EXPECT_FLOAT_EQ(loadedLight->light_power, 20.0f);
	EXPECT_EQ(loadedLight->light_color, glm::vec3(0.2f, 0.5f, 1.0f));
	EXPECT_FLOAT_EQ(loadedLight->light_radius, 0.1f);
	EXPECT_EQ(loadedLight->GetPosition(), glm::vec3(-2.0f, 0.0f, 5.0f));
	EXPECT_EQ(loadedLight->o_type, ObjectID::GOType::GO_LIGHT);

	EXPECT_EQ(loadedScene.selections.GetSelectionCount(), 0u);
	EXPECT_EQ(loadedScene.selections.GetActiveObject(), nullptr);
}

// -----------------------------------------------------------------------
// Roundtrip: Selection state
// -----------------------------------------------------------------------

/**
 * @test Selection (stored as pointers, persisted as UIDs) survives a
 *       save/load roundtrip: selected set and active object are restored
 *       against the reloaded objects by UID.
 */
TEST(ProjectRoundtrip, SelectionState)
{
	const neurus::BuiltInRenderData catalog;
	TempFile tmp("test_rt_selection.neurus.json");
	int meshUid = 0;
	int lightUid = 0;
	{
		Scene scene;
		auto config = std::make_shared<RenderConfig>();
		ResourceManager resources;

		auto meshData = resources.Load<MeshData>("obj/cube.obj");
		auto mesh = resources.Load<Mesh>(meshData);
		scene.UseMesh(mesh);
		meshUid = mesh->GetObjectID();

		auto light = resources.Load<Light>(POINTLIGHT, 5.0f, glm::vec3(1.0f));
		scene.UseLight(light);
		lightUid = light->GetObjectID();

		// Select mesh (replace), then add light — light becomes active.
		scene.selections.Select(mesh.get(), false);
		scene.selections.Select(light.get(), true);

		auto p = MakeProject(scene, config, resources, catalog);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	auto loadedConfig = std::make_shared<RenderConfig>();
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources, catalog);
		p.Load(tmp.path);
	}

	// Both objects selected, light is active.
	EXPECT_EQ(loadedScene.selections.GetSelectionCount(), 2u);

	const ObjectID* loadedMesh = loadedScene.GetObjectID(meshUid);
	const ObjectID* loadedLight = loadedScene.GetObjectID(lightUid);
	ASSERT_NE(loadedMesh, nullptr);
	ASSERT_NE(loadedLight, nullptr);
	EXPECT_TRUE(loadedScene.selections.IsSelected(loadedMesh));
	EXPECT_TRUE(loadedScene.selections.IsSelected(loadedLight));

	const ObjectID* active = loadedScene.selections.GetActiveObject();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), lightUid);
}

namespace {

class ProjectOpenSink final : public RenderRequestSink
{
public:
	void Post(RenderRequest&& request) override
	{
		if (auto* compile = std::get_if<CompileRenderGraphRequest>(&request))
			publishedGraph = compile->graph;
	}

	std::shared_ptr<const RenderGraphData> publishedGraph;
};

// The Debug log records successful reads/decodes and individual shaderc calls.
// Distinct paths and a unique shader name identify the fixture's resources even
// if the editor loads additional resources of its own.
#if defined(_DEBUG) || !defined(NDEBUG)
void ExpectContentLoads(uint64_t since, const char* phase, const std::array<int, 4>& expected)
{
	struct Probe { const char* property; const char* prefix; const char* resource; };
	const std::array<Probe, 4> probes{{
		{"shared_obj", "[MeshData] loaded '", "sphere.obj"},
		{"unused_obj", "[MeshData] loaded '", "monkey.obj"},
		{"unused_image", "[ImageData] Loaded LDR: ", "rough.png"},
		{"inactive_shader", "[ShaderCompiler] Compiling ", "BaselineInactiveCompose"},
	}};
	std::array<int, 4> counts{};
	const auto& log = LogBuffer::instance();
	const auto end = log.HeadSeq();
	for (auto seq = since + 1; seq <= end; ++seq)
	{
		LogEntry entry;
		ASSERT_TRUE(log.BySeq(seq, entry)) << "Missing log entry during " << phase;
		for (size_t i = 0; i < probes.size(); ++i)
		{
			if (entry.message.starts_with(probes[i].prefix)
			    && entry.message.find(probes[i].resource) != std::string::npos)
				++counts[i];
		}
	}
	for (size_t i = 0; i < counts.size(); ++i)
	{
		const auto property = std::string(phase) + "_" + probes[i].property;
		testing::Test::RecordProperty(property, counts[i]);
		EXPECT_EQ(counts[i], expected[i]) << property;
	}
}
#endif

} // namespace

// Cereal writes an exact shared_ptr<UID> with a static-type marker and no type name.
TEST(ProjectRoundtrip, IndexRestoresPlainUidAlongsideNamedDerivedTypes)
{
	const BuiltInRenderData catalog;
	ResourceManager source;
	auto plain = source.Load<UID>();
	auto camera = source.Load<Camera>();
	std::ostringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		project::ResourceComponent(source, catalog).Save(archive);
	}

	ResourceManager restored;
	const int nextUid = UID::GetTotalAllocated();
	project::ResourceComponent(restored, catalog).LoadJson(serialized.str());
	ASSERT_EQ(restored.Size(), 2u);
	const auto base = restored.Get(plain->GetObjectID());
	ASSERT_TRUE(base);
	const UID& object = *base;
	EXPECT_TRUE(typeid(object) == typeid(UID));
	const auto named = restored.GetByName<Camera>(camera->o_name);
	ASSERT_TRUE(named);
	EXPECT_EQ(named->GetObjectID(), camera->GetObjectID());
	EXPECT_EQ(UID::GetTotalAllocated(), nextUid);
}

TEST(ProjectRoundtrip, MixedPoolContentLoads)
{
	TempFile projectFile("test_mixed_pool.neurus.json");
	TempFile savedCopy("test_mixed_pool_saved.neurus.json");
	ApplicationCore app;
	Editor writer(nullptr, app.RenderDefaults());
	writer.NewScene();
	auto& source = writer.GetResourceManager();
	auto& scene = writer.GetScene();
	const auto camera = source.Load<Camera>();
	scene.UseCamera(camera);
	scene.ActivateCamera(camera->GetObjectID());
	const auto geometry = source.Load<MeshData>("res/obj/sphere.obj");
	const auto first = source.Load<Mesh>(geometry);
	const auto second = source.Load<Mesh>(geometry);
	scene.UseMesh(first);
	scene.UseMesh(second);
	scene.selections.Select(second.get(), false);
	const auto unusedMesh = source.Load<MeshData>("res/obj/cornellbox/monkey.obj");
	const auto unusedImage = source.Load<ImageData>("res/tex/rough.png");
	ASSERT_GT(unusedMesh->GetVertexCount(), 0u);
	ASSERT_TRUE(unusedImage->IsValid());

	const auto& ports = app.RenderDefaults()->Ports();
	const auto activeGraph = source.Load<RenderGraphData>(ports);
	writer.GetRenderManager().GraphData() = activeGraph;
	const auto inactiveGraph = source.Load<RenderGraphData>(ports);
	const auto shader = source.Load<ComputeShader>("BaselineInactiveCompose",
		ShaderData::ResolveSourcePath("res/shaders/compute/compose.comp.glsl"));
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	ASSERT_EQ(inactiveGraph->nodes.at(5)->name, "compose");
	inactiveGraph->nodes.at(5)->data.shader = shader;
	std::string error, layout;
	ASSERT_TRUE(app.SaveProject(projectFile.path, writer, layout, error)) << error;

	ProjectOpenSink sink;
	Editor reader(&sink, app.RenderDefaults());
	const auto& log = LogBuffer::instance();
	[[maybe_unused]] auto since = log.HeadSeq();
	const auto start = std::chrono::steady_clock::now();
	ASSERT_TRUE(app.LoadProject(projectFile.path, reader, layout, error)) << error;
	const auto elapsed = std::chrono::steady_clock::now() - start;
	RecordProperty("open_us", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
#if defined(_DEBUG) || !defined(NDEBUG)
	ExpectContentLoads(since, "open", {1, 0, 0, 0});
#endif
	const auto& restored = reader.GetScene();
	ASSERT_EQ(restored.mesh_list.size(), 2u);
	const auto& firstGeometry = restored.mesh_list.at(first->GetObjectID())->o_mesh;
	ASSERT_TRUE(firstGeometry);
	EXPECT_GT(firstGeometry->GetVertexCount(), 0u);
	EXPECT_EQ(firstGeometry, restored.mesh_list.at(second->GetObjectID())->o_mesh);
	EXPECT_EQ(restored.ActiveCameraID(), camera->GetObjectID());
	EXPECT_EQ(restored.selections.GetActiveObject(), restored.mesh_list.at(second->GetObjectID()).get());
	ASSERT_TRUE(sink.publishedGraph);
	EXPECT_EQ(project::ToRecord(*activeGraph), project::ToRecord(*sink.publishedGraph));
	ASSERT_TRUE(reader.GetRenderManager().GraphData());
	EXPECT_EQ(reader.GetRenderManager().GraphData()->GetObjectID(), activeGraph->GetObjectID());

	since = log.HeadSeq();
	ASSERT_TRUE(app.SaveProject(savedCopy.path, reader, layout, error)) << error;
#if defined(_DEBUG) || !defined(NDEBUG)
	ExpectContentLoads(since, "save", {0, 1, 1, 1});
#endif
	since = log.HeadSeq();
	const auto& pool = reader.GetResourceManager();
	const auto mesh = pool.Get<MeshData>(unusedMesh->GetObjectID());
	ASSERT_TRUE(mesh);
	EXPECT_GT(mesh->GetVertexCount(), 0u);
	EXPECT_EQ(mesh, pool.Get<MeshData>(unusedMesh->GetObjectID()));
	const auto image = pool.Get<ImageData>(unusedImage->GetObjectID());
	ASSERT_TRUE(image);
	EXPECT_TRUE(image->IsValid());
	const auto graph = pool.Get<RenderGraphData>(inactiveGraph->GetObjectID());
	ASSERT_TRUE(graph);
	const auto loadedShader = pool.Get<ComputeShader>(shader->GetObjectID());
	ASSERT_TRUE(loadedShader);
	EXPECT_FALSE(loadedShader->GetStage(ShaderType::COMPUTE).spv.empty());
	EXPECT_EQ(graph->nodes.at(5)->data.shader, loadedShader);
#if defined(_DEBUG) || !defined(NDEBUG)
	ExpectContentLoads(since, "lookup", {0, 0, 0, 0});
#endif
}

TEST(ProjectRoundtrip, LegacyCameraNameClaimsItsDefaultInUidOrder)
{
	const BuiltInRenderData catalog;
	ResourceManager source;
	const auto older = source.Load<Camera>();
	const auto newer = source.Load<Camera>();
	older->o_name = "NameFieldToOmit";
	newer->o_name = "Camera";

	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		project::ResourceComponent(source, catalog).Save(archive);
	}
	auto document = stream.str();
	const auto oldName = document.find("\"NameFieldToOmit\"");
	ASSERT_NE(oldName, std::string::npos);
	const auto key = document.rfind("\"o_name\"", oldName);
	ASSERT_NE(key, std::string::npos);
	document.replace(key, sizeof("\"o_name\"") - 1, "\"legacy_name\"");

	ResourceManager restored;
	project::ResourceComponent(restored, catalog).LoadJson(document);
	const auto first = restored.GetByName<Camera>("Camera");
	const auto second = restored.GetByName<Camera>("Camera.001");
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_EQ(first->GetObjectID(), older->GetObjectID());
	EXPECT_EQ(second->GetObjectID(), newer->GetObjectID());
	EXPECT_EQ(first->o_name, "Camera");
	EXPECT_EQ(second->o_name, "Camera.001");
}
