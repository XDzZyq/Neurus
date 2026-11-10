# src/platform/CMakeLists.txt - Platform abstraction layer

# Platform path and process helpers are standard-library only. They are a
# separate target so the Qt-free, Vulkan-free command transport and CLI can use
# them without inheriting the surface layer's Vulkan dependency.
add_library(neurus_platform_paths STATIC
	PlatformPaths.h
	PlatformProcess.h
)

if(WIN32)
	target_sources(neurus_platform_paths PRIVATE
		win/Win32Paths.cpp
		win/Win32Process.cpp
	)
elseif(APPLE)
	target_sources(neurus_platform_paths PRIVATE
		mac/MacOSPaths.cpp
		mac/MacOSProcess.cpp
	)
else()
	message(FATAL_ERROR "PlatformPaths and PlatformProcess have no implementation for this platform.")
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
endif()
