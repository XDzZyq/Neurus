# src/ipc/CMakeLists.txt - Loopback command transport (Qt-free, Vulkan-free)

add_library(neurus_ipc STATIC
	Frame.h
	Frame.cpp
	RandomToken.h
	RandomToken.cpp
	SessionRegistry.h
	SessionRegistry.cpp
	IpcClient.h
	IpcClient.cpp
	IpcServer.h
	IpcServer.cpp
)

target_include_directories(neurus_ipc PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_ipc PUBLIC
	neurus_command
	neurus_core
	neurus_platform_paths
	asio
)
