# src/scene/CMakeLists.txt - Scene Layer Library
#
# Contains scene graph base classes: UID, ObjectID, and eventually
# Camera, Light, Mesh, and other scene objects.
#
# Dependencies: glm (header-only math), Vulkan (via Mesh→Material include chain)
# Consumed by: editor, render, test

	add_library(neurus_scene STATIC
	Material.h
	Material.cpp
	MeshBindings.cpp
	Camera.h
	Camera.cpp
	DefaultScene.h
	DefaultScene.cpp
	Mesh.h
	Mesh.cpp
	ObjectID.h
	Transform.h
	Transform.cpp
	Light.h
	Light.cpp
	Sprite.h
	Sprite.cpp
	Scene.h
	Scene.cpp
	DebugLine.h
	DebugLine.cpp
	DebugPoints.h
	DebugPoints.cpp
	DebugMesh.h
	DebugMesh.cpp
	DebugDrawList.h
	Environment.h
	Environment.cpp
	registrations/TypeRegistration.cpp
)

target_include_directories(neurus_scene PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_scene PRIVATE
	Vulkan::Vulkan
)

target_link_libraries(neurus_scene PUBLIC
	glm
	cereal
	neurus_core
)

target_link_libraries(neurus_scene PRIVATE
	neurus_render
)

target_compile_features(neurus_scene PUBLIC cxx_std_20)
