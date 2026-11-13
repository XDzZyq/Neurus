# src/CMakeLists.txt - Main Neurus library and executable

# ---------------------------------------------------------------------------
# Layer Libraries
# ---------------------------------------------------------------------------

# Core Types (header-only, no dependencies)
add_subdirectory(core)

# Command core (Qt-free, Vulkan-free typed commands)
add_subdirectory(command)

# IPC transport (qt-free, Vulkan-free loopback command server/client)
add_subdirectory(ipc)

# Platform Layer (Vulkan surface abstraction - leaf dependency)
add_subdirectory(platform)

# Asset Layer (CPU-side data only - no internal dependencies)
add_subdirectory(asset)

# Renderer Layer (depends on: asset)
add_subdirectory(render)

# Editor Layer (depends on: nothing from src/)
add_subdirectory(editor)

# UI Layer (depends on: editor for EventBus; Qt only)
if(NEURUS_BUILD_UI)
	add_subdirectory(ui)
endif()

# Scene Layer (base types for scene graph - no internal deps)
add_subdirectory(scene)

# Application Layer (depends on: render, editor, ui, asset, scene)
add_subdirectory(app)

# Script Layer (embedded CPython + pybind11; depends on: editor, core, scene)
if(NEURUS_ENABLE_SCRIPTING)
	add_subdirectory(script)
endif()

# Standalone command-line client (Qt-free, Vulkan-free)
add_subdirectory(cli)

# ---------------------------------------------------------------------------
# Main Executable (Qt GUI; skipped by a headless-only build)
# ---------------------------------------------------------------------------

if(NEURUS_BUILD_UI)

# The executable icon comes from a resource script, which needs the RC
# language; project() enables CXX only.
if(WIN32)
	enable_language(RC)
endif()

add_executable(Neurus
	main.cpp
)

target_link_libraries(Neurus PRIVATE
	neurus_app
	neurus_render
	neurus_editor
	neurus_ui
	neurus_asset
	neurus_scene
	ads::qtadvanceddocking-qt6
	Qt6::Core
	Qt6::Quick
	Qt6::Qml
	Qt6::Widgets
)

if(NEURUS_ENABLE_SCRIPTING)
	target_link_libraries(Neurus PRIVATE neurus_script)
endif()

target_include_directories(Neurus PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)

# ADS_STATIC: tell Qt-Advanced-Docking-System headers to use static linkage
target_compile_definitions(Neurus PRIVATE ADS_STATIC)

# GLM_FORCE_DEPTH_ZERO_TO_ONE: glm::perspective() produces Vulkan [0,1] depth range
target_compile_definitions(Neurus PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)

# Windows: mark as GUI application (no console window in Release)
if(WIN32)
	set_target_properties(Neurus PROPERTIES
		WIN32_EXECUTABLE $<IF:$<CONFIG:Debug>,FALSE,TRUE>
		VS_DEBUGGER_COMMAND "$(OutDir)Neurus.exe"
		VS_DEBUGGER_WORKING_DIRECTORY "$(OutDir)"
	)

	# Application icon embedded in Neurus.exe (Explorer, shortcuts, taskbar).
	target_sources(Neurus PRIVATE "${CMAKE_SOURCE_DIR}/res/ui/logo/neurus.rc")

	# Suppress LNK4099 for pre-compiled libraries without PDBs
	target_link_options(Neurus PRIVATE /ignore:4099)

	# Deploy Qt DLLs via windeployqt (handles Core, Gui, Qml, Quick and their dependencies)
	add_custom_command(TARGET Neurus POST_BUILD
		COMMAND "${WINDEPLOYQT}"
			--no-opengl-sw
			--no-system-d3d-compiler
			--no-compiler-runtime
			--verbose 0
			$<TARGET_FILE_DIR:Neurus>
		COMMENT "Deploying Qt DLLs via windeployqt"
	)

	# Deploy the CPython runtime DLL next to the executable. Windows never
	# searches the managed environment directory (script-system design, 9).
	if(NEURUS_ENABLE_SCRIPTING)
		add_custom_command(TARGET Neurus POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${NEURUS_PYTHON_DLL}"
				"$<TARGET_FILE_DIR:Neurus>"
			COMMENT "Copying the CPython runtime DLL"
		)
	endif()
endif()

# macOS: build a real application bundle so Finder, the Dock and Cmd-Tab show
# the Neurus icon. The executable lands in Neurus.app/Contents/MacOS, which is
# still $<TARGET_FILE_DIR:Neurus>, so the res/ POST_BUILD copy below keeps
# matching QCoreApplication::applicationDirPath().
if(APPLE)
	set_target_properties(Neurus PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
		MACOSX_BUNDLE_BUNDLE_NAME Neurus
		MACOSX_BUNDLE_GUI_IDENTIFIER io.github.xdzzyq.neurus
		MACOSX_BUNDLE_ICON_FILE neurus.icns
		MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
		MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
	)

	# The bundle step copies this into Contents/Resources and CFBundleIconFile
	# points at it.
	set_source_files_properties("${CMAKE_SOURCE_DIR}/res/ui/logo/neurus.icns"
		PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	target_sources(Neurus PRIVATE "${CMAKE_SOURCE_DIR}/res/ui/logo/neurus.icns")
endif()

# Copy res/ folder to the output directory so resources resolve at runtime
# without walking up the source tree.
add_custom_command(TARGET Neurus POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_SOURCE_DIR}/res"
		"$<TARGET_FILE_DIR:Neurus>/res"
	COMMENT "Copying res/ to output directory"
)

endif() # NEURUS_BUILD_UI

# ---------------------------------------------------------------------------
# Headless renderer (no Qt, no window system)
# ---------------------------------------------------------------------------
if(NEURUS_BUILD_HEADLESS)
	add_executable(neurus_headless
		main_headless.cpp
		app/HeadlessApplication.cpp
	)

	target_link_libraries(neurus_headless PRIVATE
		neurus_app_core
		neurus_session
		neurus_render_passes
		neurus_asset
		# neurus_asset references the cereal dynamic-init anchor defined in
		# neurus_editor's registration TU, so the editor archive must come after
		# it on a single-pass GNU linker.
		neurus_editor
		neurus_render
		neurus_scene
		neurus_platform_paths
		neurus_command
		neurus_ipc
		neurus_core
		Vulkan::Vulkan
	)

	if(NEURUS_ENABLE_SCRIPTING)
		target_link_libraries(neurus_headless PRIVATE neurus_script)
	endif()

	target_include_directories(neurus_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(neurus_headless PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)

	# The CLI resolves the app as a sibling of its own executable, so both
	# binaries must land in the same directory.
	set_target_properties(neurus_headless PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:neurus_cli>"
	)

	add_custom_command(TARGET neurus_headless POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_SOURCE_DIR}/res"
			"$<TARGET_FILE_DIR:neurus_headless>/res"
		COMMENT "Copying res/ to the headless output directory"
	)
endif()
