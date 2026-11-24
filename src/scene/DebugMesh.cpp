#include "scene/DebugMesh.hpp"

#include "asset/data/MeshData.hpp"

namespace neurus
{

DebugMesh::DebugMesh()
	: ObjectID(GOType::GO_DM, "DebugMesh")
{
}

DebugMesh::DebugMesh(RestoredId id)
	: ObjectID(id, GOType::GO_DM, "DebugMesh")
{
}

DebugMesh::DebugMesh(std::shared_ptr<MeshData> meshData)
	: DebugMesh()
{
	SetMeshData(std::move(meshData));
}

void DebugMesh::SetMeshData(std::shared_ptr<MeshData> meshData)
{
	o_mesh = std::move(meshData);
}

} // namespace neurus
