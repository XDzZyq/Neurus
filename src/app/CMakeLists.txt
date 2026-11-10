# src/app/CMakeLists.txt - Application layer (lifecycle management)

add_library(neurus_app STATIC
	Application.h
	Application.cpp
	ApplicationCommands.cpp
	ApplicationSignals.cpp
	AppArgs.h
	AppArgs.cpp
	Preferences.h
	Preferences.cpp
	VulkanContext.h
	VulkanContext.cpp
	session/SessionCommandExecutor.h
	session/SessionCommandExecutor.cpp
	session/SessionServer.h
	session/SessionServer.cpp
)

target_include_directories(neurus_app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_BINARY_DIR}/generated/shaders
)

target_link_libraries(neurus_app PRIVATE
	neurus_platform
	neurus_render
	neurus_editor
	neurus_ui
	neurus_asset
	neurus_scene
	cereal
)

# Application.h exposes command::ISessionState, so the command include dirs
# must propagate to the executables and tests that include it.
target_link_libraries(neurus_app PUBLIC neurus_ipc)

# Application.cpp references ScriptEngine (owned script console buffer), so the
# dependency is declared here rather than relying on the final executables to
# list neurus_script after neurus_app.
if(NEURUS_ENABLE_SCRIPTING)
	target_link_libraries(neurus_app PRIVATE neurus_script)
endif()
