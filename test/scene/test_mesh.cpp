/**
 * @file test_mesh.cpp
 * @brief Unit tests for Mesh scene object.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * Tests cover construction, defaults, material/flags, and inheritance.
 */

#include <gtest/gtest.h>
#include "asset/ResourceManager.hpp"

#include <glm/glm.hpp>

#include "scene/Mesh.hpp"
#include "asset/data/ShaderData.hpp"
#include "scene/Transform.hpp"
#include "scene/ObjectID.hpp"
#include "asset/data/MeshData.hpp"
#include "scene/Material.hpp"

using namespace neurus;

namespace
{

/** @brief Minimal test ShaderData subclass for verifying typed SetObjShader. */
class TestShader : public ShaderData
{
public:
	TestShader() : ShaderData("test") {}
	ShaderType GetType() const override { return ShaderType::COMPUTE; }
};

} // anonymous namespace

TEST(Mesh, DefaultConstruction)
{
	neurus::ResourceManager resources;
	Mesh mesh;

	EXPECT_EQ(mesh.o_type, ObjectID::GOType::GO_MESH);
	EXPECT_EQ(mesh.o_name, "Mesh");

	EXPECT_EQ(mesh.o_material, nullptr);
	EXPECT_EQ(mesh.o_mesh.shared(), nullptr);
	EXPECT_FALSE(mesh.o_shader);

	EXPECT_TRUE(mesh.using_shadow);
	EXPECT_TRUE(mesh.using_material);
	EXPECT_TRUE(mesh.using_sdf);
	EXPECT_TRUE(mesh.is_closure);

	EXPECT_EQ(mesh.o_mesh.id(), 0);
	auto data = resources.Load<MeshData>();
	Mesh empty(data);
	EXPECT_EQ(empty.o_type, mesh.o_type);
	EXPECT_EQ(empty.o_mesh.shared(), data);
	EXPECT_EQ(empty.o_mesh.id(), data->RequireObjectID());
	EXPECT_EQ(mesh.GetMaterial(), nullptr);

	EXPECT_EQ(mesh.GetShader(), nullptr);

	void* t = mesh.GetTransform();
	EXPECT_NE(t, nullptr);
}

TEST(Mesh, PersistentGeometryRequiresPoolMembership)
{
	ResourceManager resources;
	Mesh mesh;
	const auto geometry = std::make_shared<MeshData>();
	EXPECT_THROW(mesh.o_mesh = geometry, std::invalid_argument);
	EXPECT_FALSE(mesh.o_mesh);
	const int uid = resources.Register(geometry);
	mesh.o_mesh = geometry;
	EXPECT_EQ(mesh.o_mesh.identity(), uid);
	resources.Clear();
	EXPECT_FALSE(mesh.o_mesh.identity());
}

TEST(Mesh, AssignedMaterialReceivesTypedColorParameters)
{
	Mesh mesh;
	auto material = std::make_shared<Material>();
	mesh.o_material = material;
	ASSERT_EQ(mesh.GetMaterial(), material.get());

	const glm::vec3 gold(1.0f, 0.766f, 0.336f);
	mesh.SetMatColor(Material::MAT_METAL, 0.75f);
	mesh.SetMatColor(Material::MAT_ALBEDO, gold);
	EXPECT_FLOAT_EQ(std::get<1>(material->mat_params.at(Material::MAT_METAL)), 0.75f);
	EXPECT_EQ(std::get<2>(material->mat_params.at(Material::MAT_ALBEDO)), gold);
}

TEST(Mesh, FeatureSwitchesToggleIndependently)
{
	Mesh mesh;

	EXPECT_TRUE(mesh.using_shadow);

	mesh.EnableShadow(false);
	EXPECT_FALSE(mesh.using_shadow);
	EXPECT_TRUE(mesh.using_material);
	EXPECT_TRUE(mesh.using_sdf);

	mesh.EnableShadow(true);
	EXPECT_TRUE(mesh.using_shadow);

	EXPECT_TRUE(mesh.using_material);

	mesh.EnableMaterial(false);
	EXPECT_FALSE(mesh.using_material);
	EXPECT_TRUE(mesh.using_shadow);
	EXPECT_TRUE(mesh.using_sdf);

	mesh.EnableMaterial(true);
	EXPECT_TRUE(mesh.using_material);

	EXPECT_TRUE(mesh.using_sdf);

	mesh.EnableSDF(false);
	EXPECT_FALSE(mesh.using_sdf);
	EXPECT_TRUE(mesh.using_shadow);
	EXPECT_TRUE(mesh.using_material);

	mesh.EnableSDF(true);
	EXPECT_TRUE(mesh.using_sdf);
}

TEST(Mesh, MaterialEditsOnAnUnassignedMeshAreSafe)
{
	Mesh mesh;

	// No material assigned - calling SetMatColor should be safe
	mesh.SetMatColor(Material::MAT_ALBEDO, glm::vec3(1.0f));
	mesh.SetMatColor(Material::MAT_METAL, 0.5f);

	// No material - should be safe (no-op or error-free)
	mesh.SetTex(Material::MAT_NORMAL, "dummy.png");
}

// -----------------------------------------------------------------------
// SetObjShader
// -----------------------------------------------------------------------

/**
 * @brief o_shader is null by default and SetObjShader stores the provided shader.
 */
TEST(Mesh, SetObjShader)
{
	neurus::ResourceManager resources;
	Mesh mesh;
	EXPECT_FALSE(mesh.o_shader);

	auto shader = resources.Load<TestShader>();
	mesh.SetObjShader(shader);
	EXPECT_EQ(mesh.o_shader.get(), shader.get());
	EXPECT_EQ(mesh.GetShader(), shader.get());
}
