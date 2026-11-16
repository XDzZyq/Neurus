# src/cli/CMakeLists.txt - Standalone Neurus command-line client

# Parsing, output and command implementations live in a library so the CPU-only
# test target can exercise them without linking an executable.
add_library(neurus_cli_core STATIC
	CliMain.hpp
	Cli.cpp
	CliParse.hpp
	CliParse.cpp
	Output.hpp
	Output.cpp
	AppArguments.hpp
	AppArguments.cpp
	SessionClient.hpp
	SessionClient.cpp
	TerminalBinding.hpp
	TerminalBinding.cpp
	CommandClient.hpp
	CommandClient.cpp
	SubCommand.hpp
	SubCommand.cpp
	subcommands/OpenCommand.hpp
	subcommands/OpenCommand.cpp
	subcommands/CloseCommand.hpp
	subcommands/CloseCommand.cpp
	subcommands/StatusCommand.hpp
	subcommands/StatusCommand.cpp
	subcommands/ListCommand.hpp
	subcommands/ListCommand.cpp
	subcommands/ProjectCommand.hpp
	subcommands/ProjectCommand.cpp
	subcommands/SceneCommand.hpp
	subcommands/SceneCommand.cpp
	subcommands/ObjectCommand.hpp
	subcommands/ObjectCommand.cpp
	subcommands/RenderCommand.hpp
	subcommands/RenderCommand.cpp
	subcommands/HistoryCommand.hpp
	subcommands/HistoryCommand.cpp
	subcommands/ScriptCommand.hpp
	subcommands/ScriptCommand.cpp
	subcommands/SnapshotCommand.hpp
	subcommands/SnapshotCommand.cpp
	subcommands/UseCommand.hpp
	subcommands/UseCommand.cpp
)

target_include_directories(neurus_cli_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_cli_core PUBLIC
	neurus_ipc
	neurus_platform_paths
	cli11
	nlohmann_json
)

add_executable(neurus_cli main.cpp)
set_target_properties(neurus_cli PROPERTIES OUTPUT_NAME neurus)
target_link_libraries(neurus_cli PRIVATE neurus_cli_core)

# Keep the CLI out of the build output root: Windows is case-insensitive and
# Neurus.exe already lives there.
if(CMAKE_CONFIGURATION_TYPES)
	set_target_properties(neurus_cli PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cli"
		RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/cli"
		RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/cli"
		RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo/cli"
		RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel/cli")
else()
	set_target_properties(neurus_cli PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cli")
endif()
