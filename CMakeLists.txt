cmake_minimum_required(VERSION 3.27)
project(Neurus VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++ Standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# MSVC Settings
# ---------------------------------------------------------------------------
if(MSVC)
	add_compile_options(/W4 /utf-8 /MP)
	# Every TU that includes <cereal/archives/json.hpp> + <cereal/types/polymorphic.hpp>
	# emits one COMDAT section per instantiated function. The registration TUs
	# (editor/operations/registrations, scene/registrations) are all-macro files
	# with nothing to fold, so they blow past MSVC's default 65535-section COFF
	# limit (error C1128) as soon as a few more types are registered. /bigobj only
	# widens that limit -- no codegen or ABI change -- and cereal builds itself the
	# same way (see dep/cereal/CMakeLists.txt).
	add_compile_options(/bigobj)
	# Suppress MSVC-specific noisy warnings for third-party headers
	add_compile_options(/wd4100)  # unreferenced formal parameter
	add_compile_options(/wd4127)  # conditional expression is constant
	add_compile_options(/wd4189)  # local variable is initialized but not referenced
	add_compile_options(/wd4201)  # nonstandard extension: nameless struct/union
	add_compile_options(/wd4324)  # structure was padded due to alignment specifier
endif()

# ---------------------------------------------------------------------------
# Project Options
# ---------------------------------------------------------------------------
option(NEURUS_BUILD_TESTS "Build unit tests" ON)
option(NEURUS_ENABLE_VALIDATION "Enable Vulkan validation layers" OFF)

# ---------------------------------------------------------------------------
# Debug macro (uniform across compilers)
# ---------------------------------------------------------------------------
# MSVC's CRT auto-defines _DEBUG in Debug builds; Clang/GCC do not. Define it
# ourselves so NEURUS_LOG and any other _DEBUG-guarded code behave the same on
# every platform.
add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# GLM (header-only math library)
add_subdirectory(dep/glm EXCLUDE_FROM_ALL)
target_include_directories(glm INTERFACE dep/glm)

# STB (header-only image library - stub, only stb_image.h needed later)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE dep/stb)

# Cereal (header-only serialization library)
add_library(cereal INTERFACE)
target_include_directories(cereal INTERFACE dep/cereal/include)

# Vulkan (from SDK)
find_package(Vulkan REQUIRED)
if(NOT Vulkan_FOUND)
	message(FATAL_ERROR "Vulkan SDK not found. Set VULKAN_SDK environment variable.")
endif()

# Qt6 (window, QML, EventBus)
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick Qml Widgets Test)

# ---------------------------------------------------------------------------
# Pre-compiled / Source Dependency Resolution
# ---------------------------------------------------------------------------
# Dependencies.cmake provides neurus_find_dependency() which prefers
# pre-compiled libraries in lib/<platform>/<name>/ over source builds in
# dep/<name>/. Run `make update` to download pre-compiled binaries.
# ---------------------------------------------------------------------------
include(cmake/Dependencies.cmake)
neurus_detect_platform()

neurus_find_dependency(qtadvanceddocking)
neurus_find_dependency(shaderc)

# Locate windeployqt for DLL deployment (Windows only)
if(WIN32)
	find_program(WINDEPLOYQT windeployqt
		HINTS "${Qt6_DIR}/../../../bin"  # e.g. D:/Qt/6.11.1/msvc2022_64/bin
		DOC "Path to windeployqt executable"
		REQUIRED
	)
	message(STATUS "Found windeployqt: ${WINDEPLOYQT}")
endif()



# ---------------------------------------------------------------------------
# Output Directories
# ---------------------------------------------------------------------------
# Place executables directly in build/Debug/ (or build/Release/) instead of
# mirroring the source tree under build/src/Debug/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------


add_subdirectory(src)

if(NEURUS_BUILD_TESTS)
	enable_testing()
	add_subdirectory(dep/googletest EXCLUDE_FROM_ALL)
	add_subdirectory(test)
endif()
