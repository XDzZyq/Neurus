#include "asset/data/MeshData.h"
#include "asset/data/AssetPath.h"

#include "core/Log.h"
#include "core/Timer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction / content reload
// ---------------------------------------------------------------------------

MeshData::MeshData(const std::string& path)
	: m_path(path)
{
	ReloadContent();
}

void MeshData::ReloadContent()
{
	if (m_path.empty())
		return;

	const std::string resolved = ResolveAssetPath(m_path);
	if (!LoadObj(resolved))
	{
		NEURUS_ERR("[MeshData] ReloadContent failed to load: " << resolved);
	}
}

// ---------------------------------------------------------------------------
// Helper: resolve OBJ index (1-based or negative relative → 0-based)
// ---------------------------------------------------------------------------

/**
 * @brief Converts a 1-based or negative relative OBJ index to 0-based.
 * @param rawIndex Raw index from a face token.
 * @param count Number of elements in the corresponding array.
 * @return 0-based index into the array.
 */
static int32_t ResolveIndex(int32_t rawIndex, size_t count)
{
	if (rawIndex > 0)
	{
		return rawIndex - 1;
	}
	else if (rawIndex < 0)
	{
		return static_cast<int32_t>(count) + rawIndex;
	}
	else
	{
		return rawIndex;  // 0 is technically invalid in OBJ, handle gracefully
	}
}

// ---------------------------------------------------------------------------
// Helper: parse a face vertex token (e.g. "1/2/3", "1//3", "1/2", "1")
// ---------------------------------------------------------------------------

/**
 * @brief Parses a single OBJ face vertex reference token.
 * @param token Token string ("v", "v/vt", "v//vn" or "v/vt/vn").
 * @param outPos Output position index (0-based, -1 if absent).
 * @param outUV Output UV index (0-based, -1 if absent).
 * @param outNorm Output normal index (0-based, -1 if absent).
 */
static void ParseFaceVertex(const std::string& token,
                            int32_t& outPos,
                            int32_t& outUV,
                            int32_t& outNorm)
{
	outPos = -1;
	outUV = -1;
	outNorm = -1;

	size_t slashCount = 0;
	for (char c : token)
	{
		if (c == '/')
			++slashCount;
	}

	if (slashCount == 0)
	{
		outPos = std::stoi(token);
	}
	else if (slashCount == 1)
	{
		auto slashPos = token.find('/');
		std::string a = token.substr(0, slashPos);
		std::string b = token.substr(slashPos + 1);
		outPos = std::stoi(a);
		if (!b.empty())
			outUV = std::stoi(b);
	}
	else if (slashCount == 2)
	{
		auto firstSlash = token.find('/');
		auto secondSlash = token.find('/', firstSlash + 1);
		std::string a = token.substr(0, firstSlash);
		std::string b = token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
		std::string c = token.substr(secondSlash + 1);
		outPos = std::stoi(a);
		if (!b.empty())
			outUV = std::stoi(b);
		if (!c.empty())
			outNorm = std::stoi(c);
	}
}

// ---------------------------------------------------------------------------
// Helper: write vertex attributes into the byte array
// ---------------------------------------------------------------------------

static void WriteVertex(std::vector<float>& data,
                        const glm::vec3& pos,
                        const glm::vec3& norm,
                        const glm::vec2& uv,
                        const glm::vec3& tangent,
                        const glm::vec3& bitangent)
{
	data.push_back(pos.x);
	data.push_back(pos.y);
	data.push_back(pos.z);
	data.push_back(norm.x);
	data.push_back(norm.y);
	data.push_back(norm.z);
	data.push_back(uv.x);
	data.push_back(uv.y);
	data.push_back(tangent.x);
	data.push_back(tangent.y);
	data.push_back(tangent.z);
	data.push_back(bitangent.x);
	data.push_back(bitangent.y);
	data.push_back(bitangent.z);
}

// ---------------------------------------------------------------------------
// MeshData implementation
// ---------------------------------------------------------------------------

void MeshData::AddFace(const std::vector<std::string>& faceTokens,
                  const std::vector<glm::vec3>& positions,
                  const std::vector<glm::vec2>& texCoords,
                  const std::vector<glm::vec3>& normals,
                  bool hasUVs,
                  bool hasNormals)
{
	const size_t numVerts = faceTokens.size();
	if (numVerts < 3)
		return;

	// Triangulate via fan: (0,1,2), (0,2,3), ... (0,k,k+1)

	struct FaceVert
	{
		int32_t posIdx;
		int32_t uvIdx;
		int32_t normIdx;
	};
	std::vector<FaceVert> parsed(numVerts);

	for (size_t i = 0; i < numVerts; ++i)
	{
		ParseFaceVertex(faceTokens[i], parsed[i].posIdx, parsed[i].uvIdx, parsed[i].normIdx);

		parsed[i].posIdx = ResolveIndex(parsed[i].posIdx, positions.size());
		if (hasUVs && parsed[i].uvIdx >= -1)
			parsed[i].uvIdx = ResolveIndex(parsed[i].uvIdx, texCoords.size());
		if (hasNormals && parsed[i].normIdx >= -1)
			parsed[i].normIdx = ResolveIndex(parsed[i].normIdx, normals.size());
	}

	for (size_t t = 1; t < numVerts - 1; ++t)
	{
		size_t idx[3] = { 0, t, t + 1 };

		for (size_t corner = 0; corner < 3; ++corner)
		{
			const auto& fv = parsed[idx[corner]];

			int32_t pi = std::max(0, std::min(fv.posIdx, static_cast<int32_t>(positions.size() - 1)));
			int32_t ui = hasUVs ? std::max(0, std::min(fv.uvIdx, static_cast<int32_t>(texCoords.size() - 1))) : -1;
			int32_t ni = hasNormals ? std::max(0, std::min(fv.normIdx, static_cast<int32_t>(normals.size() - 1))) : -1;

			MeshData::VertexKey key{ pi, ui, ni };

			auto it = me_vertexMap.find(key);
			if (it != me_vertexMap.end())
			{
				me_meshData.indexArray.push_back(it->second);
			}
			else
			{
				uint32_t newIdx = me_nextIndex++;
				me_vertexMap[key] = newIdx;

				glm::vec3 pos = positions[pi];
				glm::vec3 norm = (ni >= 0) ? normals[ni] : glm::vec3(0.0f);
				glm::vec2 uv = (ui >= 0) ? texCoords[ui] : glm::vec2(0.0f);

				WriteVertex(me_meshData.dataArray, pos, norm, uv,
				            glm::vec3(0.0f),   // tangent - computed later
				            glm::vec3(0.0f));  // bitangent - computed later

				me_meshData.indexArray.push_back(newIdx);
			}
		}
	}
}

void MeshData::ComputeFaceNormals()
{
	const size_t indexCount = me_meshData.indexArray.size();
	auto& data = me_meshData.dataArray;

	for (size_t i = 0; i + 2 < indexCount; i += 3)
	{
		uint32_t i0 = me_meshData.indexArray[i];
		uint32_t i1 = me_meshData.indexArray[i + 1];
		uint32_t i2 = me_meshData.indexArray[i + 2];

		glm::vec3 p0(data[i0 * 14 + 0], data[i0 * 14 + 1], data[i0 * 14 + 2]);
		glm::vec3 p1(data[i1 * 14 + 0], data[i1 * 14 + 1], data[i1 * 14 + 2]);
		glm::vec3 p2(data[i2 * 14 + 0], data[i2 * 14 + 1], data[i2 * 14 + 2]);

		glm::vec3 edge1 = p1 - p0;
		glm::vec3 edge2 = p2 - p0;
		glm::vec3 faceNormal = glm::normalize(glm::cross(edge1, edge2));

		for (size_t corner = 0; corner < 3; ++corner)
		{
			uint32_t vi = me_meshData.indexArray[i + corner];
			data[vi * 14 + 3] = faceNormal.x;
			data[vi * 14 + 4] = faceNormal.y;
			data[vi * 14 + 5] = faceNormal.z;
		}
	}
}

void MeshData::ComputeTangents()
{
	const size_t indexCount = me_meshData.indexArray.size();
	auto& data = me_meshData.dataArray;

	for (size_t i = 0; i + 2 < indexCount; i += 3)
	{
		uint32_t i0 = me_meshData.indexArray[i];
		uint32_t i1 = me_meshData.indexArray[i + 1];
		uint32_t i2 = me_meshData.indexArray[i + 2];

		glm::vec3 p0(data[i0 * 14 + 0], data[i0 * 14 + 1], data[i0 * 14 + 2]);
		glm::vec3 p1(data[i1 * 14 + 0], data[i1 * 14 + 1], data[i1 * 14 + 2]);
		glm::vec3 p2(data[i2 * 14 + 0], data[i2 * 14 + 1], data[i2 * 14 + 2]);

		glm::vec2 uv0(data[i0 * 14 + 6], data[i0 * 14 + 7]);
		glm::vec2 uv1(data[i1 * 14 + 6], data[i1 * 14 + 7]);
		glm::vec2 uv2(data[i2 * 14 + 6], data[i2 * 14 + 7]);

		glm::vec3 edge1 = p1 - p0;
		glm::vec3 edge2 = p2 - p0;
		glm::vec2 deltaUV1 = uv1 - uv0;
		glm::vec2 deltaUV2 = uv2 - uv0;

		float denom = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;

		glm::vec3 tangent(0.0f);
		if (std::abs(denom) > 1e-6f)
		{
			float f = 1.0f / denom;
			tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
			tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
			tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);

			if (glm::length(tangent) > 0.0f)
				tangent = glm::normalize(tangent);
		}
		else
		{
			// Degenerate UVs - fallback to edge direction
			if (glm::length(edge1) > 0.0f)
				tangent = glm::normalize(edge1);
		}

		for (size_t corner = 0; corner < 3; ++corner)
		{
			uint32_t vi = me_meshData.indexArray[i + corner];
			glm::vec3 normal(data[vi * 14 + 3], data[vi * 14 + 4], data[vi * 14 + 5]);
			glm::vec3 bitangent = glm::cross(normal, tangent);

			data[vi * 14 + 8]  = tangent.x;
			data[vi * 14 + 9]  = tangent.y;
			data[vi * 14 + 10] = tangent.z;
			data[vi * 14 + 11] = bitangent.x;
			data[vi * 14 + 12] = bitangent.y;
			data[vi * 14 + 13] = bitangent.z;
		}
	}
}

void MeshData::ComputeCenter()
{
	if (me_rawPositions.empty())
	{
		me_meshData.center = glm::vec3(0.0f);
		return;
	}

	glm::vec3 sum(0.0f);
	for (const auto& p : me_rawPositions)
	{
		sum += p;
	}
	me_meshData.center = sum / static_cast<float>(me_rawPositions.size());
}

// --- Public API ---

bool MeshData::LoadObj(const std::string& path)
{
	NEURUS_TIMER("mesh load '" + path + "'");
	std::ifstream file(path);
	if (!file.is_open())
	{
		NEURUS_ERR("MeshData::LoadObj failed to open: " << path);
		return false;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	bool ok = LoadObjFromString(buffer.str());
	if (ok)
	{
		NEURUS_LOG("[MeshData] loaded '" << me_meshData.name
		          << "' from " << path
		          << " - " << GetVertexCount() << " verts, "
		          << GetIndexCount() << " indices");
	}
	return ok;
}

bool MeshData::LoadObjFromString(const std::string& objContent)
{
	// --- Reset state ---
	me_meshData = ByteArray{};
	me_vertexMap.clear();
	me_rawPositions.clear();
	me_nextIndex = 0;

	// --- Temporary storage for OBJ data ---
	std::vector<glm::vec3> positions;
	std::vector<glm::vec2> texCoords;
	std::vector<glm::vec3> normals;

	bool hasUVs = false;
	bool hasNormals = false;

	std::istringstream stream(objContent);
	std::string line;

	while (std::getline(stream, line))
	{
		size_t start = line.find_first_not_of(" \t\r");
		if (start == std::string::npos)
			continue;

		if (line[start] == '#')
			continue;

		std::string trimmed = line.substr(start);
		std::istringstream lineStream(trimmed);
		std::string prefix;
		lineStream >> prefix;

		if (prefix == "v")
		{
			// Vertex position
			float x, y, z;
			lineStream >> x >> y >> z;
			if (!lineStream.fail())
			{
				positions.push_back(glm::vec3(x, y, z));
				me_rawPositions.push_back(glm::vec3(x, y, z));
			}
			else
			{
				// Optional w component (ignore)
				float w;
				lineStream.clear();
				lineStream >> w;
				lineStream >> x >> y >> z;
				if (!lineStream.fail())
				{
					positions.push_back(glm::vec3(x, y, z));
					me_rawPositions.push_back(glm::vec3(x, y, z));
				}
			}
		}
		else if (prefix == "vt")
		{
			// Texture coordinate
			float u, v;
			lineStream >> u >> v;
			if (!lineStream.fail())
			{
				texCoords.push_back(glm::vec2(u, v));
				hasUVs = true;
			}
			else
			{
				// Optional w component (3D texture)
				float w;
				lineStream.clear();
				lineStream >> w;
				lineStream >> u >> v;
				if (!lineStream.fail())
				{
					texCoords.push_back(glm::vec2(u, v));
					hasUVs = true;
				}
			}
		}
		else if (prefix == "vn")
		{
			// Vertex normal
			float nx, ny, nz;
			lineStream >> nx >> ny >> nz;
			if (!lineStream.fail())
			{
				normals.push_back(glm::vec3(nx, ny, nz));
				hasNormals = true;
			}
		}
		else if (prefix == "f")
		{
			// Face
			std::vector<std::string> faceTokens;
			std::string token;
			while (lineStream >> token)
			{
				faceTokens.push_back(token);
			}

			if (faceTokens.size() >= 3)
			{
				AddFace(faceTokens, positions, texCoords, normals, hasUVs, hasNormals);
			}
		}
		else if (prefix == "o")
		{
			// Object name
			std::string namePart;
			std::string fullName;
			while (lineStream >> namePart)
			{
				if (!fullName.empty())
					fullName += " ";
				fullName += namePart;
			}
			me_meshData.name = fullName;
		}
		// Other prefixes (s, g, usemtl, mtllib, etc.) are silently ignored
	}

	// --- Post-processing ---

	if (!hasNormals)
	{
		ComputeFaceNormals();
	}

	ComputeTangents();

	ComputeCenter();

	NEURUS_LOG("[MeshData] parsed '" << me_meshData.name
	          << "' - " << GetVertexCount() << " verts, "
	          << GetIndexCount() << " indices, "
	          << "center=(" << me_meshData.center.x << ", "
	          << me_meshData.center.y << ", " << me_meshData.center.z << ")");
	return true;
}

const MeshData::ByteArray& MeshData::GetMeshData() const
{
	return me_meshData;
}

glm::vec3 MeshData::GetMeshCenter() const
{
	return me_meshData.center;
}

std::string MeshData::GetMeshName() const
{
	return me_meshData.name;
}

size_t MeshData::GetVertexCount() const
{
	return me_meshData.dataArray.size() / 14;
}

size_t MeshData::GetIndexCount() const
{
	return me_meshData.indexArray.size();
}

} // namespace neurus
