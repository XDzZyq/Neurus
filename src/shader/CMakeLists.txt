# CPU shader processing; no Vulkan device or Qt dependency.
add_library(neurus_shader STATIC
	BuiltInRenderData.hpp
	BuiltInRenderData.cpp
	RenderShader.hpp
	RenderShader.cpp
	ComputeShader.hpp
	ComputeShader.cpp
	ShaderParser.hpp
	ShaderParser.cpp
	ShaderGenerator.hpp
	ShaderGenerator.cpp
	ShaderCompiler.hpp
	ShaderCompiler.cpp
	ShaderLibrary.hpp
	ShaderLibrary.cpp
	registrations/ShaderRegistration.hpp
	registrations/ShaderRegistration.cpp
)
target_include_directories(neurus_shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(neurus_shader PUBLIC neurus_asset_data PRIVATE shaderc_shared)
target_compile_features(neurus_shader PUBLIC cxx_std_20)
