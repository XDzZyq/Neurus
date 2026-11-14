/**
 * @file test_mesh.cpp
 * @brief Unit tests for Mesh scene object.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * Tests cover construction, defaults, material/flags, and inheritance.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include "scene/Mesh.h"
#include "asset/data/shader/ShaderData.h"
#include "scene/Transform.h"
#include "scene/ObjectID.h"
#include "asset/data/MeshData.h"
#include "scene/Material.h"

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

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

/**
 * @brief Default-constructed Mesh has correct type and name.
 */
TEST(Mesh, DefaultConstruction)
{
	Mesh mesh;
	EXPECT_EQ(mesh.o_type, ObjectID::GOType::GO_MESH);
	EXPECT_EQ(mesh.o_name, "Mesh");
}

/**
 * @brief Default-constructed Mesh has null resource pointers.
 */
TEST(Mesh, DefaultResourcesAreNull)
{
	Mesh mesh;
	EXPECT_EQ(mesh.o_material, nullptr);
	EXPECT_EQ(mesh.o_mesh, nullptr);
	EXPECT_EQ(mesh.o_shader, nullptr);
}

/**
 * @brief Default flags are all true.
 */
TEST(Mesh, DefaultFlagsAreTrue)
{
	Mesh mesh;
	EXPECT_TRUE(mesh.using_shadow);
	EXPECT_TRUE(mesh.using_material);
	EXPECT_TRUE(mesh.using_sdf);
	EXPECT_TRUE(mesh.is_closure);
}

// -----------------------------------------------------------------------
// ObjectID inheritance
// -----------------------------------------------------------------------

/**
 * @brief Mesh inherits from ObjectID and gets a unique ID.
 */
TEST(Mesh, InheritsObjectID)
{
	Mesh mesh;
	// Each mesh gets a unique, non-negative ID
	EXPECT_GE(mesh.GetObjectID(), 0);
}

/**
 * @brief Two meshes have distinct IDs.
 */
TEST(Mesh, DistinctIDs)
{
	Mesh a;
	Mesh b;
	EXPECT_NE(a.GetObjectID(), b.GetObjectID());
}

// -----------------------------------------------------------------------
// Transform3D inheritance
// -----------------------------------------------------------------------

/**
 * @brief Mesh inherits Transform3D and exposes model matrix.
 */
TEST(Mesh, InheritsTransform3D)
{
	Mesh mesh;
	// Default identity matrix
	EXPECT_EQ(mesh.GetModelMatrix(), glm::mat4(1.0f));

	// Set position and verify transform is applied
	mesh.SetPosition(glm::vec3(5.0f, 10.0f, -3.0f));
	glm::mat4 model = mesh.GetModelMatrix();
	EXPECT_FLOAT_EQ(model[3][0], 5.0f);
	EXPECT_FLOAT_EQ(model[3][1], 10.0f);
	EXPECT_FLOAT_EQ(model[3][2], -3.0f);
}

// -----------------------------------------------------------------------
// GetShader / GetMaterial / GetTransform overrides
// -----------------------------------------------------------------------

/**
 * @brief GetTransform returns a non-null Transform pointer.
 */
TEST(Mesh, GetTransformReturnsValid)
{
	Mesh mesh;
	void* t = mesh.GetTransform();
	EXPECT_NE(t, nullptr);
}

/**
 * @brief GetMaterial returns nullptr when no material assigned.
 */
TEST(Mesh, GetMaterialNullByDefault)
{
	Mesh mesh;
	EXPECT_EQ(mesh.GetMaterial(), nullptr);
}

/**
 * @brief GetShader returns nullptr when no shader assigned.
 */
TEST(Mesh, GetShaderNullByDefault)
{
	Mesh mesh;
	EXPECT_EQ(mesh.GetShader(), nullptr);
}

// -----------------------------------------------------------------------
// Material assignment
// -----------------------------------------------------------------------

/**
 * @brief Assigning a material makes GetMaterial return non-null.
 */
TEST(Mesh, SetMaterial)
{
	Mesh mesh;
	auto mat = std::make_shared<Material>();
	mesh.o_material = mat;
	EXPECT_EQ(mesh.GetMaterial(), mat.get());
}

// -----------------------------------------------------------------------
// Flag toggling
// -----------------------------------------------------------------------

/**
 * @brief EnableShadow toggles the shadow flag.
 */
TEST(Mesh, EnableShadowToggles)
{
	Mesh mesh;
	EXPECT_TRUE(mesh.using_shadow);

	mesh.EnableShadow(false);
	EXPECT_FALSE(mesh.using_shadow);

	mesh.EnableShadow(true);
	EXPECT_TRUE(mesh.using_shadow);
}

/**
 * @brief EnableMaterial toggles the material flag.
 */
TEST(Mesh, EnableMaterialToggles)
{
	Mesh mesh;
	EXPECT_TRUE(mesh.using_material);

	mesh.EnableMaterial(false);
	EXPECT_FALSE(mesh.using_material);

	mesh.EnableMaterial(true);
	EXPECT_TRUE(mesh.using_material);
}

/**
 * @brief EnableSDF toggles the SDF flag.
 */
TEST(Mesh, EnableSDFToggles)
{
	Mesh mesh;
	EXPECT_TRUE(mesh.using_sdf);

	mesh.EnableSDF(false);
	EXPECT_FALSE(mesh.using_sdf);

	mesh.EnableSDF(true);
	EXPECT_TRUE(mesh.using_sdf);
}

// -----------------------------------------------------------------------
// SetMatColor
// -----------------------------------------------------------------------

/**
 * @brief SetMatColor with float value updates material parameter.
 */
TEST(Mesh, SetMatColorFloat)
{
	Mesh mesh;
	auto mat = std::make_shared<Material>();
	mesh.o_material = mat;

	mesh.SetMatColor(Material::MAT_METAL, 0.75f);

	// Verify the material parameter was updated
	const auto& params = mat->mat_params;
	auto it = params.find(Material::MAT_METAL);
	ASSERT_NE(it, params.end());
	EXPECT_FLOAT_EQ(std::get<1>(it->second), 0.75f);
}

/**
 * @brief SetMatColor with vec3 value updates material parameter.
 */
TEST(Mesh, SetMatColorVec3)
{
	Mesh mesh;
	auto mat = std::make_shared<Material>();
	mesh.o_material = mat;

	glm::vec3 gold(1.0f, 0.766f, 0.336f);
	mesh.SetMatColor(Material::MAT_ALBEDO, gold);

	const auto& params = mat->mat_params;
	auto it = params.find(Material::MAT_ALBEDO);
	ASSERT_NE(it, params.end());
	glm::vec3 stored = std::get<2>(it->second);
	EXPECT_FLOAT_EQ(stored.x, gold.x);
	EXPECT_FLOAT_EQ(stored.y, gold.y);
	EXPECT_FLOAT_EQ(stored.z, gold.z);
}

/**
 * @brief SetMatColor with no material does not crash.
 */
TEST(Mesh, SetMatColorNoMaterialNoCrash)
{
	Mesh mesh;
	// No material assigned - calling SetMatColor should be safe
	mesh.SetMatColor(Material::MAT_ALBEDO, glm::vec3(1.0f));
	mesh.SetMatColor(Material::MAT_METAL, 0.5f);
	// Reaching this line means no crash
	SUCCEED();
}

// -----------------------------------------------------------------------
// SetObjShader
// -----------------------------------------------------------------------

/**
 * @brief o_shader is null by default and SetObjShader stores the provided shader.
 */
TEST(Mesh, SetObjShader)
{
	Mesh mesh;
	EXPECT_EQ(mesh.o_shader, nullptr);

	auto shader = std::make_shared<TestShader>();
	mesh.SetObjShader(shader);
	EXPECT_EQ(mesh.o_shader, shader);
	EXPECT_EQ(mesh.GetShader(), shader.get());
}

// -----------------------------------------------------------------------
// SetTex
// -----------------------------------------------------------------------

/**
 * @brief SetTex with no material does not crash.
 */
TEST(Mesh, SetTexNoMaterialNoCrash)
{
	Mesh mesh;
	// No material - should be safe (no-op or error-free)
	mesh.SetTex(Material::MAT_NORMAL, "dummy.png");
	SUCCEED();
}

// -----------------------------------------------------------------------
// OBJ-path constructor
// -----------------------------------------------------------------------

/**
 * @brief Constructing Mesh with empty string produces valid mesh with no data.
 */
TEST(Mesh, ConstructorEmptyMeshData)
{
	// An empty MeshData reference produces a valid mesh with no geometry.
	Mesh mesh(std::make_shared<MeshData>());
	EXPECT_EQ(mesh.o_type, ObjectID::GOType::GO_MESH);
	EXPECT_EQ(mesh.o_meshDataId, 0);
	// MeshData may or may not be loaded depending on error handling
	// The key requirement is no crash and valid Mesh object
	SUCCEED();
}

// -----------------------------------------------------------------------
// Visibility (inherited from ObjectID)
// -----------------------------------------------------------------------

/**
 * @brief Mesh is visible in viewport and rendering by default.
 */
TEST(Mesh, DefaultVisibility)
{
	Mesh mesh;
	EXPECT_TRUE(mesh.is_viewport);
	EXPECT_TRUE(mesh.is_rendered);
}
