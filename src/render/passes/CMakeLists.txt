# src/render/passes/CMakeLists.txt - Render Pass Library

add_library(neurus_render_passes STATIC
	ComposePass.h
	ComposePass.cpp
	ComputePass.h
	DebugPass.h
	DebugPass.cpp
	FXAAPass.h
	FXAAPass.cpp
	GeometryPass.h
	GeometryPass.cpp
	IBLPass.h
	IBLPass.cpp
	LightingPass.h
	LightingPass.cpp
	Pass.h
	Pass.cpp
	SelectionOutlinePass.h
	SelectionOutlinePass.cpp
	ShadowDepthPass.h
	ShadowDepthPass.cpp
	ShadowIntensityPass.h
	ShadowIntensityPass.cpp
	SSAOPass.h
	SSAOPass.cpp
	SyncObjects.h
)

target_link_libraries(neurus_render_passes PUBLIC
	Vulkan::Vulkan
	Qt6::Gui
	glm
	stb
	neurus_core
	neurus_scene
)

target_include_directories(neurus_render_passes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(neurus_render_passes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_include_directories(neurus_render_passes PRIVATE ${CMAKE_BINARY_DIR}/generated/shaders)

# GLM_FORCE_DEPTH_ZERO_TO_ONE: glm::ortho/perspective() produce Vulkan [0,1] depth range
target_compile_definitions(neurus_render_passes PRIVATE
	GLM_FORCE_DEPTH_ZERO_TO_ONE
	NEURUS_SHADER_DIR="${CMAKE_SOURCE_DIR}/res/shaders/"
)
