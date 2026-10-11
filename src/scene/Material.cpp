#include "Material.h"

namespace neurus {

// ---------------------------------------------------------------------------
// Static member definition
// ---------------------------------------------------------------------------

std::vector<std::string> Material::mat_uniform_name;

// ---------------------------------------------------------------------------
// Constructor / InitParamData
// ---------------------------------------------------------------------------

Material::Material()
{
	InitParamData();
}

void Material::InitParamData()
{
	// One-time init of the shared uniform-name table.
	if (mat_uniform_name.empty())
	{
		mat_uniform_name.resize(MAT_END);
		mat_uniform_name[MAT_ALBEDO]   = "mat_albedo";
		mat_uniform_name[MAT_METAL]    = "mat_metal";
		mat_uniform_name[MAT_ROUGH]    = "mat_rough";
		mat_uniform_name[MAT_SPEC]     = "mat_spec";
		mat_uniform_name[MAT_EMIS_COL] = "mat_emis_col";
		mat_uniform_name[MAT_EMIS_STR] = "mat_emis_str";
		mat_uniform_name[MAT_ALPHA]    = "mat_alpha";
		mat_uniform_name[MAT_NORMAL]   = "mat_normal";
		mat_uniform_name[MAT_BUMP]     = "mat_bump";
	}

	// Seed every parameter with sensible defaults
	mat_params.clear();

	//                   type        float      colour             texture (nullptr)
	mat_params[MAT_ALBEDO]   = { MPARA_COL, 0.0f,      glm::vec3(1.0f, 1.0f, 1.0f), nullptr };
	mat_params[MAT_METAL]    = { MPARA_FLT, 0.0f,      glm::vec3(0.0f),              nullptr };
	mat_params[MAT_ROUGH]    = { MPARA_FLT, 0.5f,      glm::vec3(0.0f),              nullptr };
	mat_params[MAT_SPEC]     = { MPARA_COL, 0.0f,      glm::vec3(0.04f),             nullptr };
	mat_params[MAT_EMIS_COL] = { MPARA_COL, 0.0f,      glm::vec3(0.0f),              nullptr };
	mat_params[MAT_EMIS_STR] = { MPARA_FLT, 0.0f,      glm::vec3(0.0f),              nullptr };
	mat_params[MAT_ALPHA]    = { MPARA_FLT, 1.0f,      glm::vec3(0.0f),              nullptr };
	mat_params[MAT_NORMAL]   = { MPARA_TEX, 0.0f,      glm::vec3(0.0f),              nullptr };
	mat_params[MAT_BUMP]     = { MPARA_TEX, 0.0f,      glm::vec3(0.0f),              nullptr };

	is_mat_changed = true;
	is_mat_struct_changed = true;
}

// ---------------------------------------------------------------------------
// SetMatParam overloads
// ---------------------------------------------------------------------------

void Material::SetMatParam(MatParaType _tar, MatDataType _type)
{
	auto it = mat_params.find(_tar);
	if (it == mat_params.end())
	{
		return;
	}

	std::get<0>(it->second) = _type;

	is_mat_changed = true;
	is_mat_struct_changed = true;
}

void Material::SetMatParam(MatParaType _tar, float _var)
{
	auto it = mat_params.find(_tar);
	if (it == mat_params.end())
	{
		return;
	}

	std::get<0>(it->second) = MPARA_FLT;
	std::get<1>(it->second) = _var;

	is_mat_changed = true;
}

void Material::SetMatParam(MatParaType _tar, const glm::vec3& _col)
{
	auto it = mat_params.find(_tar);
	if (it == mat_params.end())
	{
		return;
	}

	std::get<0>(it->second) = MPARA_COL;
	std::get<2>(it->second) = _col;

	is_mat_changed = true;
}

void Material::SetMatParam(MatParaType _tar, std::shared_ptr<Texture> _tex)
{
	auto it = mat_params.find(_tar);
	if (it == mat_params.end())
	{
		return;
	}

	std::get<0>(it->second) = MPARA_TEX;
	std::get<3>(it->second) = std::move(_tex);

	is_mat_changed = true;
	is_mat_struct_changed = true;
}

// ---------------------------------------------------------------------------
// LoadMaterial / ParseConfig
// ---------------------------------------------------------------------------

Material::MaterialRes Material::LoadMaterial(std::string /*_path*/)
{
	// Stub - returns a default-constructed material.
	return std::make_shared<Material>();
}

void Material::ParseConfig(const std::string& /*_config*/)
{
	// Stub - no parsing is performed yet.
}

// ---------------------------------------------------------------------------
// BindMatTexture
// ---------------------------------------------------------------------------

void Material::BindMatTexture() const
{
	// Stub - texture binding is not implemented yet.
}

} // namespace neurus
