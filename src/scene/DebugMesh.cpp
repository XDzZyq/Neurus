#include "scene/DebugMesh.h"

#include "asset/data/MeshData.h"

namespace neurus
{

DebugMesh::DebugMesh()
	: ObjectID()
{
	o_type = ObjectID::GOType::GO_DM;
	o_name = "DebugMesh";
}

DebugMesh::DebugMesh(std::shared_ptr<MeshData> meshData)
	: DebugMesh()
{
	SetMeshData(std::move(meshData));
}

void DebugMesh::SetMeshData(std::shared_ptr<MeshData> meshData)
{
	o_mesh = std::move(meshData);
	// Keep the serialized ID in lockstep with the pointer; 0 means "no
	// geometry", matching Mesh and the ResourceComponent re-wiring contract.
	o_meshDataId = o_mesh ? o_mesh->GetObjectID() : 0;
}

} // namespace neurus
