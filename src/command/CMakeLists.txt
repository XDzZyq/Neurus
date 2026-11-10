# src/command/CMakeLists.txt - Generic command core (Qt-free, Vulkan-free)

add_library(neurus_command STATIC
	CommandError.h
	CommandError.cpp
	CommandTypes.h
	CommandDescriptor.h
	CommandDescriptor.cpp
	CommandDispatcher.h
	CommandDispatcher.cpp
	CommandJson.h
	CommandJson.cpp
	ICommandExecutor.h
	IEditorCommands.h
	IScriptCommands.h
	Commands.h
	ISessionState.h
)

target_include_directories(neurus_command PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_command PUBLIC
	neurus_core
	nlohmann_json
)
