# src/platform/CMakeLists.txt - Platform abstraction layer

# Platform path, process, file and system helpers need only the standard
# library plus the OS SDK. They are a separate target so the Qt-free,
# Vulkan-free command transport and CLI can use them without inheriting the
# surface layer's Vulkan dependency.
add_library(neurus_platform_paths STATIC
	PlatformPaths.h
	PlatformProcess.h
	PlatformFile.h
	PlatformCrypto.h
	PlatformSystem.h
)

if(WIN32)
	target_sources(neurus_platform_paths PRIVATE
		win/Win32Paths.cpp
		win/Win32Process.cpp
		win/Win32File.cpp
		win/Win32Crypto.cpp
		win/Win32System.cpp
	)
	target_link_libraries(neurus_platform_paths PUBLIC bcrypt shell32)
elseif(APPLE)
	target_sources(neurus_platform_paths PRIVATE
		mac/MacOSPaths.cpp
		mac/MacOSProcess.cpp
		mac/MacOSFile.cpp
		mac/MacOSCrypto.cpp
		mac/MacOSSystem.cpp
	)
elseif(UNIX AND NOT APPLE)
	# Linux headless: POSIX implementations, no window system.
	target_sources(neurus_platform_paths PRIVATE
		linux/LinuxPaths.cpp
		linux/LinuxProcess.cpp
		linux/LinuxFile.cpp
		linux/LinuxCrypto.cpp
		linux/LinuxSystem.cpp
	)
else()
	message(FATAL_ERROR "Platform helpers have no implementation for this platform.")
endif()

target_include_directories(neurus_platform_paths PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(neurus_platform STATIC
	PlatformSurface.h
)

target_link_libraries(neurus_platform PUBLIC
	Vulkan::Vulkan
	neurus_platform_paths
)
target_include_directories(neurus_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(WIN32)
	target_sources(neurus_platform PRIVATE
		win/Win32Surface.cpp
	)
	target_compile_definitions(neurus_platform PUBLIC VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
	target_sources(neurus_platform PRIVATE
		mac/MacOSSurface.mm
	)
	target_compile_definitions(neurus_platform PUBLIC VK_USE_PLATFORM_METAL_EXT)
	target_link_libraries(neurus_platform PRIVATE
		"-framework AppKit"
		"-framework QuartzCore"
		"-framework Metal"
	)
elseif(UNIX AND NOT APPLE)
	# Headless stub: no surface extension, no window system. See LinuxSurface.cpp.
	target_sources(neurus_platform PRIVATE
		linux/LinuxSurface.cpp
	)
endif()
