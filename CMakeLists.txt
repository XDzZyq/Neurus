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
	# same way (see lib/src/cereal/CMakeLists.txt).
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
option(NEURUS_ENABLE_SCRIPTING "Embed CPython scripting" ON)

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
add_subdirectory(lib/src/glm EXCLUDE_FROM_ALL)
target_include_directories(glm INTERFACE lib/src/glm)

# STB (header-only image library - stub, only stb_image.h needed later)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE lib/src/stb)

# Cereal (header-only serialization library)
add_library(cereal INTERFACE)
target_include_directories(cereal INTERFACE lib/src/cereal/include)

# Vulkan (from SDK)
find_package(Vulkan REQUIRED)
if(NOT Vulkan_FOUND)
	message(FATAL_ERROR "Vulkan SDK not found. Set VULKAN_SDK environment variable.")
endif()

# Qt6 (window, QML, EventBus)
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick Qml Widgets Test)

# ---------------------------------------------------------------------------
# Scripting (embedded CPython + pybind11)
# ---------------------------------------------------------------------------
# The uv-managed environment under lib/ (owned by Neurus-Lib) reuses the base
# interpreter's DLL and headers (PEP 405), so we link the base CPython and
# attach the environment at runtime (script-system design, sections 8 and 9).
if(NEURUS_ENABLE_SCRIPTING)
	# Managed environment. Overridable for release bundling or another env.
	set(NEURUS_PYTHON_ENV "${CMAKE_SOURCE_DIR}/lib/.venv" CACHE PATH
		"Path to the Neurus-managed Python environment (uv venv)")

	if(WIN32)
		set(Python_EXECUTABLE "${NEURUS_PYTHON_ENV}/Scripts/python.exe")
	else()
		set(Python_EXECUTABLE "${NEURUS_PYTHON_ENV}/bin/python")
	endif()

	# Resolve the base prefix up front: it is both the FindPython root (a venv
	# ships no headers or import libraries) and the runtime PyConfig home.
	execute_process(
		COMMAND "${Python_EXECUTABLE}" -c "import sys;print(sys.base_prefix)"
		OUTPUT_VARIABLE NEURUS_PYTHON_HOME
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE NEURUS_PYTHON_HOME_RESULT
		ERROR_QUIET
	)
	if(NOT NEURUS_PYTHON_HOME_RESULT EQUAL 0 OR NEURUS_PYTHON_HOME STREQUAL "")
		message(FATAL_ERROR
			"NEURUS_ENABLE_SCRIPTING is ON but '${Python_EXECUTABLE}' could not be run. "
			"Run 'make update' to provision lib/.venv, or set -DNEURUS_PYTHON_ENV=<path>.")
	endif()
	# Normalize to forward slashes so the value is a valid C string literal.
	file(TO_CMAKE_PATH "${NEURUS_PYTHON_HOME}" NEURUS_PYTHON_HOME)

	# STANDARD keeps an active VIRTUAL_ENV variable from steering the search;
	# Python_ROOT_DIR points at the base install that owns the headers/libraries.
	set(Python_FIND_VIRTUALENV STANDARD)
	set(Python_ROOT_DIR "${NEURUS_PYTHON_HOME}")
	find_package(Python REQUIRED COMPONENTS Development.Embed)

	# pybind11 comes from the env (no extra submodule): ask the interpreter
	# where its CMake config lives, and fall back to the conventional layout.
	execute_process(
		COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
		OUTPUT_VARIABLE NEURUS_PYBIND11_CMAKEDIR
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE NEURUS_PYBIND11_QUERY_RESULT
		ERROR_QUIET
	)
	if(NEURUS_PYBIND11_QUERY_RESULT EQUAL 0 AND IS_DIRECTORY "${NEURUS_PYBIND11_CMAKEDIR}")
		set(pybind11_DIR "${NEURUS_PYBIND11_CMAKEDIR}" CACHE PATH
			"pybind11 CMake package directory" FORCE)
	else()
		set(pybind11_DIR "${NEURUS_PYTHON_ENV}/Lib/site-packages/pybind11/share/cmake/pybind11"
			CACHE PATH "pybind11 CMake package directory" FORCE)
	endif()
	find_package(pybind11 CONFIG REQUIRED)

	# The CPython runtime DLL lives in the base prefix root. Windows never
	# searches the environment directory, so copy it next to each executable.
	if(WIN32)
		set(NEURUS_PYTHON_DLL
			"${NEURUS_PYTHON_HOME}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll")
	endif()

	message(STATUS "Neurus scripting: Python ${Python_VERSION} (base: ${NEURUS_PYTHON_HOME})")

	# Generated home for the embedded interpreter (see ScriptEngineImpl.h).
	file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/script")
	configure_file(
		"${CMAKE_SOURCE_DIR}/src/script/neurus_python_config.h.in"
		"${CMAKE_BINARY_DIR}/generated/script/neurus_python_config.h"
		@ONLY
	)
else()
	message(STATUS "Neurus scripting disabled (NEURUS_ENABLE_SCRIPTING=OFF)")
endif()


# ---------------------------------------------------------------------------
# Pre-compiled / Source Dependency Resolution
# ---------------------------------------------------------------------------
# Dependencies.cmake provides neurus_find_dependency() which prefers
# pre-compiled libraries in lib/prebuilt/<platform>/<name>/ over source builds
# in lib/src/<name>/. Run `make update` to download pre-compiled binaries.
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
	add_subdirectory(lib/src/googletest EXCLUDE_FROM_ALL)
	add_subdirectory(test)
endif()
