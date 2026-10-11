#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <cereal/types/base_class.hpp>
#include <cereal/types/string.hpp>

#include "core/UID.h"

namespace neurus {

/**
 * @brief CPU-side mesh data parsed from OBJ files.
 *
 * Stores interleaved vertex attributes and an index buffer for indexed
 * drawing. Vertex layout (14 floats): position(3), normal(3), uv(2),
 * tangent(3), bitangent(3). Pure CPU data - no GPU resources.
 *
 * Pooled resource (UID identity): serializes its source path and re-parses
 * the OBJ on load.
 */
class MeshData : public UID
{
public:
	/** @brief Holds the parsed mesh data arrays. */
	struct ByteArray
	{
		std::vector<float> dataArray;       ///< Interleaved vertex data (14 floats per vertex)
		std::vector<uint32_t> indexArray;   ///< Index buffer (uint32_t)
		glm::vec3 center = { 0.0f, 0.0f, 0.0f };  ///< Bounding box center
		std::string name = "";              ///< Mesh / object name from OBJ
	};

	MeshData() = default;

	/**
	 * @brief Stores the source path and loads the OBJ immediately.
	 * @param path OBJ file path.
	 */
	explicit MeshData(const std::string& path);

	// Non-copyable / non-movable (UID identity).
	MeshData(const MeshData&) = delete;
	MeshData& operator=(const MeshData&) = delete;
	MeshData(MeshData&&) = delete;
	MeshData& operator=(MeshData&&) = delete;

	/**
	 * @brief Load OBJ from a file path.
	 * @param path File path to .obj file.
	 * @return true if parsing succeeded.
	 */
	bool LoadObj(const std::string& path);

	/**
	 * @brief Load OBJ from an in-memory string (useful for testing).
	 * @param objContent Full OBJ file content as a single string.
	 * @return true if parsing succeeded.
	 */
	bool LoadObjFromString(const std::string& objContent);

	/**
	 * @brief (Re)loads the OBJ content from disk.
	 *
	 * Called from the path constructor and at the end of serialize(load);
	 * no-op when the path is empty.
	 */
	void ReloadContent();

	/**
	 * @brief Cereal serialization - UID + source path, then content reload.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this), CEREAL_NVP(m_path));
		if constexpr (Archive::is_loading::value)
		{
			ReloadContent();
		}
	}

	/** @brief Read-only access to the parsed mesh data. */
	const ByteArray& GetMeshData() const;

	/** @brief Returns the bounding box center. */
	glm::vec3 GetMeshCenter() const;

	/** @brief Returns the mesh name (from OBJ 'o' lines). */
	std::string GetMeshName() const;

	/**
	 * @brief Returns the source OBJ path (serialized, relative to asset dir).
	 * @return Const reference to the stored path (empty for procedural data).
	 */
	const std::string& GetPath() const { return m_path; }

	/** @brief Get number of unique vertices in the interleaved array. */
	size_t GetVertexCount() const;

	/** @brief Get number of indices (3 per triangle). */
	size_t GetIndexCount() const;

private:
	std::string m_path;       ///< Source OBJ path (relative to asset dir; serialized)
	ByteArray me_meshData;

	// --- Parsing helpers ---

	/**
	 * @brief Triangulates a face (triangle, quad or n-gon fan) and adds its
	 *        vertices and indices.
	 * @param faceTokens Raw face tokens (e.g. "1/1/1", "2//2", "3").
	 * @param positions Parsed positions array.
	 * @param texCoords Parsed texture coordinates array.
	 * @param normals Parsed normals array.
	 * @param hasUVs Whether the file provided UVs.
	 * @param hasNormals Whether the file provided normals.
	 */
	void AddFace(const std::vector<std::string>& faceTokens,
	             const std::vector<glm::vec3>& positions,
	             const std::vector<glm::vec2>& texCoords,
	             const std::vector<glm::vec3>& normals,
	             bool hasUVs,
	             bool hasNormals);

	/**
	 * @brief Computes per-face normals (triangle-edge cross product).
	 * Called when the OBJ file provides no normals.
	 */
	void ComputeFaceNormals();

	/**
	 * @brief Compute tangent and bitangent vectors for normal mapping.
	 * Uses positions and UVs; called after normals are finalized.
	 */
	void ComputeTangents();

	/** @brief Compute the bounding box center from stored positions. */
	void ComputeCenter();

	// Deduplication key: combination of pos/uv/norm indices
	struct VertexKey
	{
		int32_t posIdx;
		int32_t uvIdx;
		int32_t normIdx;

		bool operator==(const VertexKey& other) const
		{
			return posIdx == other.posIdx && uvIdx == other.uvIdx && normIdx == other.normIdx;
		}
	};

	struct VertexKeyHash
	{
		size_t operator()(const VertexKey& k) const
		{
			size_t h = std::hash<int32_t>()(k.posIdx);
			h = h * 31 + std::hash<int32_t>()(k.uvIdx);
			h = h * 31 + std::hash<int32_t>()(k.normIdx);
			return h;
		}
	};

	// Deduplication map
	std::unordered_map<VertexKey, uint32_t, VertexKeyHash> me_vertexMap;

	// Raw position copies for bounding box computation
	std::vector<glm::vec3> me_rawPositions;

	uint32_t me_nextIndex = 0;
};

} // namespace neurus
