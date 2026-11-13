# src/app/CMakeLists.txt - Application layer (lifecycle management)

# ---------------------------------------------------------------------------
# Shared application core (Qt-free)
# ---------------------------------------------------------------------------
# Used by both the Qt GUI application and the headless renderer: argument
# parsing, preferences, project persistence and Vulkan context creation.
add_library(neurus_app_core STATIC
	AppArgs.h
	AppArgs.cpp
	Preferences.h
	Preferences.cpp
	ProjectIo.h
	ProjectIo.cpp
	VulkanContext.h
	VulkanContext.cpp
)

# Preferences::MakeScriptPolicy() is only meaningful with the script layer; the
# definition is compiled out when scripting is off.
if(NEURUS_ENABLE_SCRIPTING)
	target_compile_definitions(neurus_app_core PUBLIC NEURUS_ENABLE_SCRIPTING=1)
endif()

target_include_directories(neurus_app_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_app_core PUBLIC
	Vulkan::Vulkan
	neurus_platform
	neurus_render
	neurus_editor
	neurus_asset
	neurus_scene
	cereal
)

# ---------------------------------------------------------------------------
# Session host (Qt-free)
# ---------------------------------------------------------------------------
# The loopback command server and its command policy. Split out so the
# headless application can host the same session protocol without the GUI.
add_library(neurus_session STATIC
	session/SessionCommandExecutor.h
	session/SessionCommandExecutor.cpp
	session/SessionServer.h
	session/SessionServer.cpp
)

target_include_directories(neurus_session PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_session PUBLIC
	neurus_core
	neurus_command
	neurus_ipc
	neurus_platform_paths
)

# ---------------------------------------------------------------------------
# GUI application (Qt)
# ---------------------------------------------------------------------------
if(NEURUS_BUILD_UI)
	if(NOT NEURUS_ENABLE_SCRIPTING)
		message(FATAL_ERROR
			"The Qt application requires NEURUS_ENABLE_SCRIPTING=ON: its preferences "
			"and command handlers depend on the script policy. Build with "
			"-DNEURUS_BUILD_UI=OFF for a Qt-free headless binary.")
	endif()

	add_library(neurus_app STATIC
		Application.h
		Application.cpp
		ApplicationCommands.cpp
		ApplicationSignals.cpp
	)

	target_include_directories(neurus_app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/..
		${CMAKE_BINARY_DIR}/generated/shaders
	)

	target_link_libraries(neurus_app PRIVATE
		neurus_app_core
		neurus_session
		neurus_ui
		neurus_asset
		neurus_scene
		cereal
	)

	# Application.h exposes command::ISessionState, so the command include dirs
	# must propagate to the executables and tests that include it.
	target_link_libraries(neurus_app PUBLIC neurus_ipc)

	# Application.cpp references ScriptEngine (owned script console buffer), so
	# the dependency is declared here rather than relying on the final
	# executables to list neurus_script after neurus_app.
	if(NEURUS_ENABLE_SCRIPTING)
		target_link_libraries(neurus_app PRIVATE neurus_script)
	endif()
endif()
