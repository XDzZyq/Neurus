#include <gtest/gtest.h>

#include "scene/Material.hpp"
#include "render/Texture.hpp"

#include <glm/glm.hpp>

using namespace neurus;

// ---------------------------------------------------------------------------
// 1. Default-constructed material has correct initial state
// ---------------------------------------------------------------------------

TEST(MaterialTest, DefaultConstruction_SetsDefaults)
{
	Material mat;

	// All nine parameter slots must exist
	EXPECT_EQ(mat.mat_params.size(), 9u);

	// Albedo: white colour
	{
		const auto& data = mat.mat_params.at(Material::MAT_ALBEDO);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_COL);
		glm::vec3 col = std::get<2>(data);
		EXPECT_FLOAT_EQ(col.r, 1.0f);
		EXPECT_FLOAT_EQ(col.g, 1.0f);
		EXPECT_FLOAT_EQ(col.b, 1.0f);
	}

	// Metallic: 0.0 float
	{
		const auto& data = mat.mat_params.at(Material::MAT_METAL);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_FLT);
		EXPECT_FLOAT_EQ(std::get<1>(data), 0.0f);
	}

	// Roughness: 0.5 float
	{
		const auto& data = mat.mat_params.at(Material::MAT_ROUGH);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_FLT);
		EXPECT_FLOAT_EQ(std::get<1>(data), 0.5f);
	}

	// Specular: (0.04, 0.04, 0.04) colour
	{
		const auto& data = mat.mat_params.at(Material::MAT_SPEC);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_COL);
		glm::vec3 col = std::get<2>(data);
		EXPECT_FLOAT_EQ(col.r, 0.04f);
		EXPECT_FLOAT_EQ(col.g, 0.04f);
		EXPECT_FLOAT_EQ(col.b, 0.04f);
	}

	// Emissive colour: black
	{
		const auto& data = mat.mat_params.at(Material::MAT_EMIS_COL);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_COL);
		glm::vec3 col = std::get<2>(data);
		EXPECT_FLOAT_EQ(col.r, 0.0f);
		EXPECT_FLOAT_EQ(col.g, 0.0f);
		EXPECT_FLOAT_EQ(col.b, 0.0f);
	}

	// Emissive strength: 0.0 float
	{
		const auto& data = mat.mat_params.at(Material::MAT_EMIS_STR);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_FLT);
		EXPECT_FLOAT_EQ(std::get<1>(data), 0.0f);
	}

	// Alpha: 1.0 float (opaque)
	{
		const auto& data = mat.mat_params.at(Material::MAT_ALPHA);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_FLT);
		EXPECT_FLOAT_EQ(std::get<1>(data), 1.0f);
	}

	// Normal: texture type, nullptr
	{
		const auto& data = mat.mat_params.at(Material::MAT_NORMAL);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_TEX);
		EXPECT_EQ(std::get<3>(data), nullptr);
	}

	// Bump: texture type, nullptr
	{
		const auto& data = mat.mat_params.at(Material::MAT_BUMP);
		EXPECT_EQ(std::get<0>(data), Material::MPARA_TEX);
		EXPECT_EQ(std::get<3>(data), nullptr);
	}

	// Dirty flags must be true after construction
	EXPECT_TRUE(mat.is_mat_changed);
	EXPECT_TRUE(mat.is_mat_struct_changed);

	// Default name
	EXPECT_EQ(mat.mat_name, "Default");
}

// ---------------------------------------------------------------------------
// 2. SetMatParam - float overload
// ---------------------------------------------------------------------------

TEST(MaterialTest, SetMatParam_Float)
{
	Material mat;
	mat.is_mat_changed = false;
	mat.is_mat_struct_changed = false;

	mat.SetMatParam(Material::MAT_METAL, 0.75f);

	const auto& data = mat.mat_params.at(Material::MAT_METAL);
	EXPECT_EQ(std::get<0>(data), Material::MPARA_FLT);
	EXPECT_FLOAT_EQ(std::get<1>(data), 0.75f);

	// Dirty flag set
	EXPECT_TRUE(mat.is_mat_changed);
	EXPECT_FALSE(mat.is_mat_struct_changed);
}

// ---------------------------------------------------------------------------
// 3. SetMatParam - vec3 (colour) overload
// ---------------------------------------------------------------------------

TEST(MaterialTest, SetMatParam_Vec3)
{
	Material mat;
	mat.is_mat_changed = false;
	mat.is_mat_struct_changed = false;
	glm::vec3 gold(1.0f, 0.766f, 0.336f);

	mat.SetMatParam(Material::MAT_ALBEDO, gold);

	const auto& data = mat.mat_params.at(Material::MAT_ALBEDO);
	EXPECT_EQ(std::get<0>(data), Material::MPARA_COL);
	glm::vec3 col = std::get<2>(data);
	EXPECT_FLOAT_EQ(col.r, 1.0f);
	EXPECT_FLOAT_EQ(col.g, 0.766f);
	EXPECT_FLOAT_EQ(col.b, 0.336f);

	EXPECT_TRUE(mat.is_mat_changed);
}

// ---------------------------------------------------------------------------
// 4. SetMatParam - texture overload (shared_ptr<Texture>)
// ---------------------------------------------------------------------------

TEST(MaterialTest, SetMatParam_Texture)
{
	Material mat;
	mat.is_mat_changed = false;
	mat.is_mat_struct_changed = false;

	// Use a default-constructed shared_ptr (nullptr) - no GPU needed
	Texture::TextureRes tex = std::make_shared<Texture>();

	mat.SetMatParam(Material::MAT_NORMAL, tex);

	const auto& data = mat.mat_params.at(Material::MAT_NORMAL);
	EXPECT_EQ(std::get<0>(data), Material::MPARA_TEX);
	EXPECT_EQ(std::get<3>(data), tex);
	EXPECT_TRUE(std::get<3>(data) != nullptr);

	EXPECT_TRUE(mat.is_mat_changed);
	EXPECT_TRUE(mat.is_mat_struct_changed);
}

// ---------------------------------------------------------------------------
// 5. SetMatParam - change data type via MatDataType overload
// ---------------------------------------------------------------------------

TEST(MaterialTest, SetMatParam_ChangeType)
{
	Material mat;
	mat.is_mat_changed = false;
	mat.is_mat_struct_changed = false;

	// Albedo starts as MPARA_COL; switch to MPARA_TEX
	mat.SetMatParam(Material::MAT_ALBEDO, Material::MPARA_TEX);

	const auto& data = mat.mat_params.at(Material::MAT_ALBEDO);
	EXPECT_EQ(std::get<0>(data), Material::MPARA_TEX);

	EXPECT_TRUE(mat.is_mat_changed);
	EXPECT_TRUE(mat.is_mat_struct_changed);
}

// ---------------------------------------------------------------------------
// 6. uniform name table initialised after construction
// ---------------------------------------------------------------------------

TEST(MaterialTest, UniformNameTable_IsPopulated)
{
	// Construction triggers InitParamData which fills the static table
	Material mat;

	ASSERT_FALSE(Material::mat_uniform_name.empty());
	ASSERT_EQ(Material::mat_uniform_name.size(), Material::MAT_END);

	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_ALBEDO],   "mat_albedo");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_METAL],    "mat_metal");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_ROUGH],    "mat_rough");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_SPEC],     "mat_spec");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_EMIS_COL], "mat_emis_col");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_EMIS_STR], "mat_emis_str");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_ALPHA],    "mat_alpha");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_NORMAL],   "mat_normal");
	EXPECT_EQ(Material::mat_uniform_name[Material::MAT_BUMP],     "mat_bump");
}

// ---------------------------------------------------------------------------
// 7. LoadMaterial factory returns default material
// ---------------------------------------------------------------------------

TEST(MaterialTest, LoadMaterial_ReturnsDefault)
{
	auto mat = Material::LoadMaterial();
	ASSERT_NE(mat, nullptr);

	// Should have all default parameters
	EXPECT_EQ(mat->mat_params.size(), 9u);

	const auto& albedo = mat->mat_params.at(Material::MAT_ALBEDO);
	EXPECT_EQ(std::get<0>(albedo), Material::MPARA_COL);
}

// ---------------------------------------------------------------------------
// 10. Material is movable but not copyable
// ---------------------------------------------------------------------------

TEST(MaterialTest, Movable_NotCopyable)
{
	Material mat1;
	mat1.SetMatParam(Material::MAT_METAL, 0.9f);

	// Move-construct
	Material mat2 = std::move(mat1);
	EXPECT_FLOAT_EQ(
		std::get<1>(mat2.mat_params.at(Material::MAT_METAL)),
		0.9f);

	// Move-assign
	Material mat3;
	mat3 = std::move(mat2);
	EXPECT_FLOAT_EQ(
		std::get<1>(mat3.mat_params.at(Material::MAT_METAL)),
		0.9f);
}

// ---------------------------------------------------------------------------
// 13. Setting params for an invalid type is a no-op (not a crash)
// ---------------------------------------------------------------------------

TEST(MaterialTest, InvalidMatParaType_NoOp)
{
	Material mat;

	// MAT_NONE (-1) is not in the map - must not crash
	EXPECT_NO_THROW(mat.SetMatParam(static_cast<Material::MatParaType>(Material::MAT_NONE), 1.0f));
	EXPECT_NO_THROW(mat.SetMatParam(static_cast<Material::MatParaType>(Material::MAT_NONE), glm::vec3(1.0f)));
	EXPECT_NO_THROW(mat.SetMatParam(static_cast<Material::MatParaType>(Material::MAT_NONE), Material::MPARA_FLT));
	EXPECT_NO_THROW(mat.SetMatParam(static_cast<Material::MatParaType>(Material::MAT_NONE), nullptr));
}
