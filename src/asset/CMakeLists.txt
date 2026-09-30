# src/asset/CMakeLists.txt - Asset Layer (CPU-side data + project persistence)

add_library(neurus_asset STATIC
	Project.h
	Project.cpp
	Serializable.h
	PixelFormat.h
	components/ConfigComponent.h
	components/ConfigComponent.cpp
	components/SceneComponent.h
	components/SceneComponent.cpp
	components/UIComponent.h
	components/UIComponent.cpp
	components/HistoryComponent.h
	components/HistoryComponent.cpp
	components/EditorComponent.h
	components/EditorComponent.cpp
	components/ResourceComponent.h
	components/ResourceComponent.cpp
	registrations/DataRegistration.h
	registrations/DataRegistration.cpp
	data/ImageData.h
	data/ImageData.cpp
	data/MeshData.h
	data/MeshData.cpp
	data/AssetPath.h
	data/AssetPath.cpp
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
# components/EditorComponent.cpp does the same for editor/Editor.h (the editor
# camera's pool UID).
