# src/scene/CMakeLists.txt - Scene Layer Library
#
# Contains scene graph base classes: UID, ObjectID, and eventually
# Camera, Light, Mesh, and other scene objects.
#
# Dependencies: glm (header-only math), Vulkan (via Mesh→Material include chain)
# Consumed by: editor, render, test

	add_library(neurus_scene STATIC
	Material.hpp
	Material.cpp
	MeshBindings.cpp
	Camera.hpp
	Camera.cpp
	DefaultScene.hpp
	DefaultScene.cpp
	Mesh.hpp
	Mesh.cpp
	ObjectID.hpp
	Transform.hpp
	Transform.cpp
	Light.hpp
	Light.cpp
	Sprite.hpp
	Sprite.cpp
	Scene.hpp
	Scene.cpp
	DebugLine.hpp
	DebugLine.cpp
	DebugPoints.hpp
	DebugPoints.cpp
	DebugMesh.hpp
	DebugMesh.cpp
	Environment.hpp
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
