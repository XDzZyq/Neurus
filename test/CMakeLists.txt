# test/CMakeLists.txt - Unit Tests

add_executable(neurus_test
	shared/TestVulkanShared.cpp
	shared/TestVulkanShared.h
	shared/TestMinimalSpv.h
	shared/test_main.cpp
	render/test_arraybuffer.cpp
	render/test_buffers.cpp
	render/test_buffer.cpp
	render/test_vulkan_context.cpp
	asset/test_meshdata.cpp
	asset/test_imagedata.cpp
	render/test_shader_parser.cpp
	render/test_syncobjects.cpp
	render/test_image.cpp
	render/test_descriptor.cpp
	render/test_texture.cpp
	render/test_material.cpp
	render/test_mesh.cpp
	render/test_attachments.cpp
	render/test_renderpass.cpp
	render/test_gbuffer.cpp
	render/test_ibl.cpp
	render/test_ibl_render.cpp
	render/test_lighting.cpp
	render/test_model_render.cpp
	render/test_deferred_shading.cpp
	render/test_ssao.cpp
	render/test_screenshot.cpp
	render/test_scene_wiring.cpp
	render/test_shadow_cubemap.cpp
	render/test_shadow_intensity.cpp
	render/test_sun_shadow_intensity.cpp
	render/test_multilight_shadow.cpp
	render/test_temporal_shadow.cpp
	render/test_sun_shadow_depth.cpp
	render/test_fxaa.cpp
	render/test_debug_pass.cpp
	render/test_pickpixel.cpp
	render/test_render_graph.cpp
	editor/test_camera_controller.cpp
	editor/test_debug_draw_builder.cpp
	editor/test_editor_scene_lifecycle.cpp
	editor/test_input.cpp
	editor/test_event_bus.cpp
	editor/test_event_bus_typed.cpp
	editor/test_scene_controller.cpp
	editor/test_operation_manager.cpp
	editor/test_operation_serialization.cpp
	editor/test_render_config_controller.cpp
	editor/test_shader_controller.cpp
	editor/test_shader_editor_panel.cpp
	scene/test_camera.cpp
	scene/test_mesh.cpp
	scene/test_uid.cpp
	scene/test_transform.cpp
	scene/test_light.cpp
	scene/test_sprite.cpp
	scene/test_scene.cpp
	scene/test_scene_integration.cpp
	scene/test_scene_serialize.cpp
	scene/test_debug.cpp
	asset/test_project_roundtrip.cpp
	asset/test_default_project.cpp
	asset/test_ui_component.cpp
	core/test_graph.cpp
	core/test_selections.cpp
	core/test_log_buffer.cpp
	core/test_uid.cpp
	core/test_resource_manager.cpp
	ui/test_log_model.cpp
	ui/test_i18n.cpp
	ui/test_po_catalog.cpp
	ui/test_dock_identity.cpp
	ui/test_floating_window.cpp
	app/test_preferences.cpp
	platform/test_platform_paths.cpp
)

target_link_libraries(neurus_test PRIVATE
	neurus_scene
	neurus_app
	neurus_platform
	neurus_render
	neurus_editor
	neurus_ui
	neurus_asset
	gtest
	Vulkan::Vulkan
	Qt6::Core
	Qt6::Test
	Qt6::Quick
	Qt6::Qml
)

target_include_directories(neurus_test PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/test
)

# Compute relative path from CMAKE_BINARY_DIR to source root for
# reference image resolution at CTest CWD (WORKING_DIRECTORY below).
file(RELATIVE_PATH TEST_RELATIVE_PROJECT_DIR "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")

target_compile_definitions(neurus_test PRIVATE
	TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
	TEST_RELATIVE_PROJECT_DIR="${TEST_RELATIVE_PROJECT_DIR}"
	GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# Enable Qt MOC for test files containing QObject subclasses
set_target_properties(neurus_test PROPERTIES AUTOMOC ON)

# MSVC: disable Qt's MOC warnings in test code
if(MSVC)
	target_compile_options(neurus_test PRIVATE /wd4100 /wd4189 /FS)
endif()

# Windows: deploy Qt DLLs so test executable can run standalone
# MUST come before gtest_discover_tests (POST_BUILD ordering)
if(WIN32 AND WINDEPLOYQT)
	# Qt6Test DLL is not auto-deployed by windeployqt, so we copy it manually
	file(REAL_PATH "${Qt6_DIR}/../../../bin" _qt6_bin_dir)
	add_custom_command(TARGET neurus_test POST_BUILD
		COMMAND "${WINDEPLOYQT}"
			--no-opengl-sw
			--no-system-d3d-compiler
			--no-compiler-runtime
			--verbose 0
			$<TARGET_FILE_DIR:neurus_test>
		COMMAND ${CMAKE_COMMAND} -E copy
			"${_qt6_bin_dir}/$<IF:$<CONFIG:Debug>,Qt6Testd.dll,Qt6Test.dll>"
			$<TARGET_FILE_DIR:neurus_test>
		COMMENT "Deploying Qt DLLs for test executable (including Qt6Test)"
	)
endif()

# Deploy shaderc_shared.dll alongside the test executable
if(TARGET shaderc_shared)
	add_custom_command(TARGET neurus_test POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:shaderc_shared>
			$<TARGET_FILE_DIR:neurus_test>
		COMMENT "Copying shaderc_shared.dll for runtime access"
	)
endif()

# Copy res/ to the build directory so tests can access assets from CWD
# (ctest runs from CMAKE_BINARY_DIR; no relative path probing needed)
add_custom_command(TARGET neurus_test POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_SOURCE_DIR}/res"
		"${CMAKE_BINARY_DIR}/res"
	COMMENT "Copying res/ to build directory for test access"
)

# Register tests with CTest.
# WORKING_DIRECTORY pinned to CMAKE_BINARY_DIR so res/ (copied there by
# POST_BUILD) is always resolvable relative to CWD.
include(GoogleTest)
gtest_discover_tests(neurus_test
	TEST_PREFIX "Neurus."
	DISCOVERY_TIMEOUT 30
	WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
