# src/cli/CMakeLists.txt - Standalone Neurus command-line client

# Parsing, output and command implementations live in a library so the CPU-only
# test target can exercise them without linking an executable.
add_library(neurus_cli_core STATIC
	Cli.h
	Cli.cpp
	CliParse.h
	CliParse.cpp
	Output.h
	Output.cpp
	AppArguments.h
	AppArguments.cpp
	SessionClient.h
	SessionClient.cpp
	SubCommand.h
	SubCommand.cpp
	subcommands/OpenCommand.h
	subcommands/OpenCommand.cpp
	subcommands/CloseCommand.h
	subcommands/CloseCommand.cpp
	subcommands/StatusCommand.h
	subcommands/StatusCommand.cpp
	subcommands/ListCommand.h
	subcommands/ListCommand.cpp
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
