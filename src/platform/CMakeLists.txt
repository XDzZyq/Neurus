# src/platform/CMakeLists.txt - Platform abstraction layer

add_library(neurus_platform STATIC
	PlatformSurface.h
	PlatformSurface.cpp
	PlatformPaths.h
	PlatformPaths.cpp
)

target_link_libraries(neurus_platform PUBLIC Vulkan::Vulkan)
target_include_directories(neurus_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(WIN32)
	target_sources(neurus_platform PRIVATE
		win/Win32Surface.h
		win/Win32Surface.cpp
	)
	target_compile_definitions(neurus_platform PUBLIC VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
	target_sources(neurus_platform PRIVATE
		mac/MacOSSurface.h
		mac/MacOSSurface.mm
	)
	target_compile_definitions(neurus_platform PUBLIC VK_USE_PLATFORM_METAL_EXT)
	target_link_libraries(neurus_platform PRIVATE
		"-framework AppKit"
		"-framework QuartzCore"
		"-framework Metal"
	)
endif()
