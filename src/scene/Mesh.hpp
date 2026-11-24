/**
 * @file Mesh.hpp
 * @brief 3D mesh object for renderable geometry in the scene hierarchy.
 *
 * Mesh is the primary renderable object, combining geometry, material,
 * shader and transform.
 *
 * @note Multiple meshes can share the same MeshData and Material via shared_ptr.
 */

#pragma once

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <cereal/types/base_class.hpp>

#include "core/Serialize.hpp"
#include "asset/ResourceRef.hpp"
#include "asset/data/ShaderData.hpp"
#include "scene/ObjectID.hpp"
#include "Transform.hpp"

namespace neurus
{

// --- Forward declarations -------------------------------------------------

class Material;
class MeshData;
/**
 * @brief 3D mesh object representing renderable geometry with material and transform.
 *
 * Combines MeshData (geometry), Material (surface properties), a shader
 * reference, and a Transform3D for placement. Supports shadow casting,
 * material shading and SDF generation for soft shadows.
 */
class Mesh : public ObjectID, public Transform3D
{
	static bool AcceptShader(const ShaderData& shader);

public:
	/** @brief GOType tag for runtime type discrimination. */
	static constexpr ObjectID::GOType Type = ObjectID::GOType::GO_MESH;

	/// Material defining surface properties (albedo, metallic, roughness, etc.)
	std::shared_ptr<Material> o_material;

	/// High-resolution geometry (vertex/index buffers)
	std::shared_ptr<MeshData> o_mesh;

	/// CPU-side shader for rendering this mesh (null = use default pass shader)
	ResourceRef<ShaderData, AcceptShader> o_shader;

	bool using_shadow   = true;
	bool using_material = true;
	bool using_sdf      = true;
	bool is_closure     = true;

	int o_meshDataId = 0;      ///< Pooled MeshData UID (geometry source)

	Mesh();
	/**
	 * @brief Constructs a mesh referencing pooled geometry.
	 * @param meshData Shared MeshData (pooled resource) to reference.
	 * @note Sets o_mesh and o_meshDataId.
	 * @note No path is accepted; file paths belong to the data layer
	 *       (MeshData holds the source OBJ path).
	 */
	explicit Mesh(std::shared_ptr<MeshData> meshData);
	~Mesh() override;

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)));
		// Missing shader references remain null and select the runtime built-in.
		// Save omits that reference rather than materializing a fallback resource.
		archive::Optional(ar, CEREAL_NVP(o_meshDataId));
		archive::OptionalRef(ar, "o_shaderId", o_shader);
		archive::Optional(ar, CEREAL_NVP(using_shadow), CEREAL_NVP(using_material),
		                  CEREAL_NVP(using_sdf), CEREAL_NVP(is_closure));
	}

	Mesh(const Mesh&) = delete;
	Mesh& operator=(const Mesh&) = delete;
	Mesh(Mesh&&) = delete;
	Mesh& operator=(Mesh&&) = delete;

	void SetObjShader(std::shared_ptr<ShaderData> shader);
	void SetTex(int _type, const std::string& _name);
	void SetMatColor(int _type, float _val);
	void SetMatColor(int _type, const glm::vec3& _col);
	void EnableShadow(bool _enable) { using_shadow = _enable; }
	void EnableMaterial(bool _enable) { using_material = _enable; }
	void EnableSDF(bool _enable) { using_sdf = _enable; }
	ShaderData* GetShader() override { return o_shader.get(); }
	const ShaderUnit* GetShaderUnit(int shaderType) const override;
	void* GetMaterial() override { return o_material.get(); }
	void* GetTransform() override { return static_cast<Transform*>(this); }

private:
};

} // namespace neurus
