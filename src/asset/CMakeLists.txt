# Passive CPU resource data and file access do not depend on shader tooling.
add_library(neurus_asset_data STATIC
	data/shader/ShaderSnapshot.hpp
	PixelFormat.hpp
	registrations/DataRegistration.hpp
	registrations/DataRegistration.cpp
	data/ImageData.hpp
	data/ImageData.cpp
	data/MeshData.hpp
	data/MeshData.cpp
	data/AssetPath.hpp
	data/AssetPath.cpp
	data/RenderGraphData.hpp
	data/RenderGraphData.cpp
	data/render_graph/GraphOutputTag.hpp
	data/render_graph/PortSource.hpp
	data/render_graph/PassShaders.hpp
	data/render_graph/PassShaders.cpp
	data/render_graph/CpuGraphValidation.hpp
	data/render_graph/CpuGraphValidation.cpp
	data/render_graph/PassPorts.hpp
	data/render_graph/PassPorts.cpp
	data/render_graph/PassKinds.hpp
	data/shader/ShaderStage.hpp
	data/shader/ShaderStruct.hpp
	data/shader/ShaderStruct.cpp
	data/shader/ShaderStructSerialize.hpp
	data/shader/ShaderUnit.hpp
	data/ShaderData.hpp
	data/ShaderData.cpp
)
target_link_libraries(neurus_asset_data PUBLIC glm stb neurus_core cereal)
target_include_directories(neurus_asset_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(neurus_asset_data PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(neurus_asset_data PRIVATE
	NEURUS_RES_DIR="${CMAKE_SOURCE_DIR}/res/"
	NEURUS_SHADER_DIR="${CMAKE_SOURCE_DIR}/res/shaders/"
)
target_compile_features(neurus_asset_data PUBLIC cxx_std_20)

# Project components persist CPU resources and scene state.
add_library(neurus_asset STATIC
	Project.hpp
	Project.cpp
	Serializable.hpp
	components/ConfigComponent.hpp
	components/ConfigComponent.cpp
	components/GraphComponent.hpp
	components/GraphComponent.cpp
	components/SceneComponent.hpp
	components/SceneComponent.cpp
	components/UIComponent.hpp
	components/UIComponent.cpp
	components/ResourceComponent.hpp
	components/ResourceComponent.cpp
)
target_link_libraries(neurus_asset PUBLIC neurus_asset_data neurus_scene)
target_include_directories(neurus_asset PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(neurus_asset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(neurus_asset PUBLIC cxx_std_20)
