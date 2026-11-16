# src/command/CMakeLists.txt - Generic command core (Qt-free, Vulkan-free)

add_library(neurus_command STATIC
	CommandError.hpp
	CommandError.cpp
	CommandTypes.hpp
	CommandDescriptor.hpp
	CommandDescriptor.cpp
	CommandDispatcher.hpp
	CommandDispatcher.cpp
	CommandJson.hpp
	CommandJson.cpp
	ICommandExecutor.hpp
	IEditorCommands.hpp
	IScriptCommands.hpp
	ISnapshotCommands.hpp
	Commands.hpp
	ISessionState.hpp
)

target_include_directories(neurus_command PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_command PUBLIC
	neurus_core
	nlohmann_json
)
