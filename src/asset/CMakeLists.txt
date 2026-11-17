# src/asset/CMakeLists.txt - Asset Layer (CPU-side data + project persistence)

add_library(neurus_asset STATIC
	Project.hpp
	Project.cpp
	Serializable.hpp
	PixelFormat.hpp
	components/ConfigComponent.hpp
	components/ConfigComponent.cpp
	components/GraphComponent.hpp
	components/GraphComponent.cpp
	components/SceneComponent.hpp
	components/SceneComponent.cpp
	components/UIComponent.hpp
	components/UIComponent.cpp
	components/HistoryComponent.hpp
	components/HistoryComponent.cpp
	components/EditorComponent.hpp
	components/EditorComponent.cpp
	components/ResourceComponent.hpp
	components/ResourceComponent.cpp
	registrations/DataRegistration.hpp
	registrations/DataRegistration.cpp
	data/ImageData.hpp
	data/ImageData.cpp
	data/MeshData.hpp
	data/MeshData.cpp
	data/AssetPath.hpp
	data/AssetPath.cpp
	data/render_graph/GraphOutputTag.hpp
	data/render_graph/CpuRenderGraph.hpp
	data/render_graph/RenderGraphSpec.hpp
	data/render_graph/RenderGraphSpec.cpp
	graph/PassPorts.hpp
	graph/PassPorts.cpp
	graph/PassKinds.hpp
	graph/ShaderSnapshot.hpp
	graph/ShaderSnapshot.cpp
	graph/RenderGraphCompile.hpp
	graph/RenderGraphCompile.cpp
	data/shader/ShaderStage.hpp
	data/shader/ShaderStruct.hpp
	data/shader/ShaderStruct.cpp
	data/shader/ShaderStructSerialize.hpp
	data/shader/ShaderUnit.hpp
	data/ShaderData.hpp
	data/ShaderData.cpp
)

target_link_libraries(neurus_asset PUBLIC
	glm
	stb
	neurus_core
	neurus_scene
	cereal
	neurus_render      # needed by ConfigComponent.cpp
)

target_compile_definitions(neurus_asset PRIVATE
	NEURUS_RES_DIR="${CMAKE_SOURCE_DIR}/res/"
)

target_include_directories(neurus_asset PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(neurus_asset PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${Vulkan_INCLUDE_DIRS}
)

target_compile_features(neurus_asset PUBLIC cxx_std_20)

# NOTE: components/HistoryComponent.cpp includes editor/operations headers
# (OperationManager, OperationRegistration) so undo/redo stacks can round-trip
# through the project file. That is a reverse asset->editor dependency, confined
# to that one .cpp; every final binary links neurus_editor alongside neurus_asset,
# so no extra link target is declared here.
# components/EditorComponent.cpp does the same for editor/viewport/EditorViewport.hpp
# (the viewport camera's pool UID).
