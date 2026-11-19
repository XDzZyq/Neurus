# src/render/passes/CMakeLists.txt - Render Pass Library

add_library(neurus_render_passes STATIC
	ComposePass.hpp
	ComposePass.cpp
	ComputePass.hpp
	DebugPass.hpp
	DebugPass.cpp
	FXAAPass.hpp
	FXAAPass.cpp
	GeometryPass.hpp
	GeometryPass.cpp
	GizmoPass.hpp
	GizmoPass.cpp
	IBLPass.hpp
	IBLPass.cpp
	LightingPass.hpp
	LightingPass.cpp
	Pass.hpp
	Pass.cpp
	SelectionOutlinePass.hpp
	SelectionOutlinePass.cpp
	ShadowDepthPass.hpp
	ShadowDepthPass.cpp
	ShadowIntensityPass.hpp
	ShadowIntensityPass.cpp
	SSAOPass.hpp
	SSAOPass.cpp
	SyncObjects.hpp
)

target_link_libraries(neurus_render_passes PUBLIC
	neurus_shader
	Vulkan::Vulkan
	glm
	stb
	neurus_core
	neurus_scene
	neurus_asset
	neurus_render_api
)

target_include_directories(neurus_render_passes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(neurus_render_passes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_include_directories(neurus_render_passes PRIVATE ${CMAKE_BINARY_DIR}/generated/shaders)

# GLM_FORCE_DEPTH_ZERO_TO_ONE: glm::ortho/perspective() produce Vulkan [0,1] depth range
target_compile_definitions(neurus_render_passes PRIVATE
	GLM_FORCE_DEPTH_ZERO_TO_ONE
	NEURUS_SHADER_DIR="${CMAKE_SOURCE_DIR}/res/shaders/"
)
