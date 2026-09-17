# ---------------------------------------------------------------------------
# Neurus Dependencies - Pre-compiled Binary Library Resolution
#
# Priority:
#   1. lib/<platform>/<dep>/  (pre-compiled, fast)
#   2. dep/<dep>/             (source build, fallback)
#   3. find_package()         (system SDK)
#
# Functions:
#   neurus_detect_platform()         - set NEURUS_PLATFORM (windows/linux/macos)
#   neurus_find_dependency(name)     - resolve a dep from lib/ or dep/
# ---------------------------------------------------------------------------

function(neurus_detect_platform)
	if(WIN32)
		set(NEURUS_PLATFORM "windows" PARENT_SCOPE)
	elseif(APPLE)
		set(NEURUS_PLATFORM "macos" PARENT_SCOPE)
	elseif(UNIX)
		set(NEURUS_PLATFORM "linux" PARENT_SCOPE)
	else()
		message(FATAL_ERROR "Unsupported platform. Neurus currently supports Windows, Linux, and macOS.")
	endif()
endfunction()

# ---------------------------------------------------------------------------
# neurus_find_dependency(name)
#
# Resolves a dependency, preferring pre-compiled binaries in lib/<platform>/<name>/
# over source builds in dep/<name>/.
#
# For each dependency, the function:
#   1. Checks if lib/<platform>/<name>/lib/ contains the pre-compiled library
#   2. If found: creates an IMPORTED target pointing to the binary
#   3. If not found: falls back to add_subdirectory(dep/<name>)
#
# The expected lib/ layout for each dependency:
#   lib/<platform>/<name>/
#     lib/           - .lib / .dll / .so files
#     include/       - (optional) build-specific headers
#
# Headers are sourced from dep/<name>/ for API headers (which don't change
# between source and pre-compiled builds).
#
# Supported dependencies:
#   shaderc           - shared library (shaderc_shared)
#   qtadvanceddocking - static library (qtadvanceddocking-qt6), pre-compiled
#                       only when its build_info.txt stamp matches this build's
#                       Qt version and C++ standard; otherwise built from source
#
# Why shaderc needs no such check: it exposes a C API and links no Qt, so its
# binary does not depend on the Qt headers we compile against. ADS is a Qt C++
# library and shares Qt's inline container template instantiations with our own
# translation units — see the note in the qtadvanceddocking branch.
# ---------------------------------------------------------------------------

option(NEURUS_ADS_FROM_SOURCE
	"Ignore any pre-compiled qtadvanceddocking archive and build ADS from source" OFF)

# ---------------------------------------------------------------------------
# neurus_ads_prebuilt_usable(<lib_dir> <out_ok> <out_reason>)
#
# A pre-compiled ADS archive is only safe to link when it was compiled against
# the same Qt headers and C++ standard as this build (see the note in the
# qtadvanceddocking branch below). cmake/ads_standalone stamps every archive it
# produces with a flat key=value build_info.txt recording both; this checks it.
#
# The stamp is parsed, never included(), so nothing from a downloaded artifact
# is ever executed as CMake code.
# ---------------------------------------------------------------------------
function(neurus_ads_prebuilt_usable lib_dir out_ok out_reason)

	set(${out_ok} FALSE PARENT_SCOPE)
	set(stamp "${lib_dir}/build_info.txt")

	if(NOT EXISTS "${stamp}")
		set(${out_reason} "no build_info.txt stamp beside the archive" PARENT_SCOPE)
		return()
	endif()

	set(stamp_qt "")
	set(stamp_std "")
	file(STRINGS "${stamp}" stamp_lines REGEX "^[a-z_]+=")
	foreach(line IN LISTS stamp_lines)
		if(line MATCHES "^qt_version=(.+)$")
			set(stamp_qt "${CMAKE_MATCH_1}")
		elseif(line MATCHES "^cxx_standard=(.+)$")
			set(stamp_std "${CMAKE_MATCH_1}")
		endif()
	endforeach()

	if(NOT stamp_qt OR NOT stamp_std)
		set(${out_reason} "unreadable stamp ${stamp}" PARENT_SCOPE)
		return()
	endif()
	if(NOT Qt6_VERSION)
		set(${out_reason} "this build's Qt version is unknown" PARENT_SCOPE)
		return()
	endif()
	if(NOT stamp_qt STREQUAL "${Qt6_VERSION}")
		set(${out_reason} "archive was built against Qt ${stamp_qt}, this build uses Qt ${Qt6_VERSION}" PARENT_SCOPE)
		return()
	endif()
	if(NOT stamp_std STREQUAL "${CMAKE_CXX_STANDARD}")
		set(${out_reason} "archive was built as C++${stamp_std}, this build is C++${CMAKE_CXX_STANDARD}" PARENT_SCOPE)
		return()
	endif()

	set(${out_ok} TRUE PARENT_SCOPE)
	set(${out_reason} "Qt ${stamp_qt}, C++${stamp_std}" PARENT_SCOPE)

endfunction()

function(neurus_find_dependency name)

	# --- Determine paths ---
	set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${NEURUS_PLATFORM}/${name}")
	set(DEP_DIR "${CMAKE_SOURCE_DIR}/dep/${name}")

	# -----------------------------------------------------------------------
	# shaderc (shared library)
	# -----------------------------------------------------------------------
	if(name STREQUAL "shaderc")

		# shaderc headers are in the dep/ submodule
		set(SHADERC_INCLUDE_DIR "${DEP_DIR}/libshaderc/include")

		if(EXISTS "${LIB_DIR}/lib/shaderc_shared.lib")
			message(STATUS "Using pre-compiled shaderc from ${LIB_DIR}")

			# Create IMPORTED shared library target
			# DLL is config-independent — same binary for all build types
			add_library(shaderc_shared SHARED IMPORTED)
			set_target_properties(shaderc_shared PROPERTIES
				IMPORTED_LOCATION "${LIB_DIR}/lib/shaderc_shared.dll"
				IMPORTED_IMPLIB   "${LIB_DIR}/lib/shaderc_shared.lib"
			)

			# Provide include dirs so downstream targets can #include <shaderc/shaderc.h>
			set_property(TARGET shaderc_shared PROPERTY
				INTERFACE_INCLUDE_DIRECTORIES "${SHADERC_INCLUDE_DIR}"
			)

			# Mark that this dependency was resolved from lib/
			set(NEURUS_DEP_${name}_FROM_LIB TRUE PARENT_SCOPE)

		elseif(APPLE AND EXISTS "${LIB_DIR}/lib/libshaderc_shared.dylib")
			message(STATUS "Using pre-compiled shaderc from ${LIB_DIR}")

			add_library(shaderc_shared SHARED IMPORTED)
			set_target_properties(shaderc_shared PROPERTIES
				IMPORTED_LOCATION "${LIB_DIR}/lib/libshaderc_shared.dylib"
			)
			set_property(TARGET shaderc_shared PROPERTY
				INTERFACE_INCLUDE_DIRECTORIES "${SHADERC_INCLUDE_DIR}"
			)
			set(NEURUS_DEP_${name}_FROM_LIB TRUE PARENT_SCOPE)

		else()
			message(STATUS "Pre-compiled shaderc not found in ${LIB_DIR}, building from source")

			# Source build from dep/shaderc (existing behavior)
			set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
			set(SHADERC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
			set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
			set(SHADERC_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
			set(ENABLE_OPT OFF CACHE BOOL "Enable spirv-opt capability" FORCE)
			add_subdirectory("${DEP_DIR}")

			# Include dirs are set by the source build's CMakeLists.txt
			set(NEURUS_DEP_${name}_FROM_LIB FALSE PARENT_SCOPE)
		endif()

	# -----------------------------------------------------------------------
	# qtadvanceddocking (static library, pre-compiled only when the stamp matches)
	# -----------------------------------------------------------------------
	elseif(name STREQUAL "qtadvanceddocking")

		# ADS is a Qt C++ library, so its objects carry coalesced
		# (linkonce_odr) instantiations of Qt's inline container templates —
		# the same symbols our own translation units emit. The linker keeps ONE
		# definition of each for the whole binary, so a pre-compiled archive is
		# only safe when it was built against the identical Qt headers and C++
		# standard as this build.
		#
		# History, for why this is checked rather than assumed: an archive built
		# against Homebrew Qt 6.11.1 was linked into TUs compiled against CI's
		# Qt 6.11.2, which had rewritten QtPrivate::QPodArrayOps<T> from
		# "derives from QArrayDataPointer<T>" to "holds a DataPointer *m_ptr"
		# while keeping the mangled names identical. A 6.11.1 caller reached the
		# surviving 6.11.2 body, which read `this` as a pointer-to-pointer and
		# dereferenced the QArrayData header's {ref_ = 1, flags = 0}: SIGSEGV at
		# address 0x1 inside ADS's dockWidgets()/openedDockWidgets(). Debug only
		# — -O3 inlines these templates per TU, so Release never interposed. A
		# mismatch must therefore fail at configure time, not at runtime.
		if(WIN32)
			set(ADS_LIB_RELEASE "${LIB_DIR}/lib/qtadvanceddocking-qt6_static.lib")
			set(ADS_LIB_DEBUG   "${LIB_DIR}/lib/qtadvanceddocking-qt6d_static.lib")
		else()
			set(ADS_LIB_RELEASE "${LIB_DIR}/lib/libqtadvanceddocking-qt6_static.a")
			set(ADS_LIB_DEBUG   "${LIB_DIR}/lib/libqtadvanceddocking-qt6d_static.a")
		endif()

		set(ADS_USE_PREBUILT FALSE)
		set(ADS_REASON "")
		if(NEURUS_ADS_FROM_SOURCE)
			set(ADS_REASON "NEURUS_ADS_FROM_SOURCE=ON")
		elseif(NOT EXISTS "${ADS_LIB_RELEASE}")
			set(ADS_REASON "no pre-compiled archive in ${LIB_DIR}")
		elseif(NOT EXISTS "${ADS_LIB_DEBUG}")
			# The Release archive is NOT substituted for a missing Debug one:
			# linking -O3 ADS objects into an -O0 binary mixes exactly the same
			# weak template symbols this check exists to protect.
			set(ADS_REASON "Debug archive missing from ${LIB_DIR}")
		else()
			neurus_ads_prebuilt_usable("${LIB_DIR}" ADS_USE_PREBUILT ADS_REASON)
		endif()

		if(ADS_USE_PREBUILT)
			message(STATUS "Using pre-compiled qtadvanceddocking from ${LIB_DIR} (${ADS_REASON})")

			add_library(qtadvanceddocking-qt6 STATIC IMPORTED)
			set_target_properties(qtadvanceddocking-qt6 PROPERTIES
				IMPORTED_LOCATION         "${ADS_LIB_RELEASE}"
				IMPORTED_LOCATION_RELEASE "${ADS_LIB_RELEASE}"
				IMPORTED_LOCATION_DEBUG   "${ADS_LIB_DEBUG}"
				MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
				MAP_IMPORTED_CONFIG_MINSIZEREL     Release
			)
			set_property(TARGET qtadvanceddocking-qt6 PROPERTY
				INTERFACE_INCLUDE_DIRECTORIES "${DEP_DIR}/src"
			)
			# The archives are built with BUILD_STATIC=ON, which defines
			# ADS_STATIC PUBLIC; consumers must agree or ADS_EXPORT resolves to
			# Q_DECL_IMPORT against a static archive.
			target_compile_definitions(qtadvanceddocking-qt6 INTERFACE ADS_STATIC)
			target_link_libraries(qtadvanceddocking-qt6 INTERFACE
				Qt6::Core Qt6::Gui Qt6::Widgets
			)
			add_library(ads::qtadvanceddocking-qt6 ALIAS qtadvanceddocking-qt6)

			set(NEURUS_DEP_${name}_FROM_LIB TRUE PARENT_SCOPE)

		else()
			message(STATUS "Building qtadvanceddocking from source: ${ADS_REASON}")

			set(ADS_VERSION "4.5.0")
			set(BUILD_EXAMPLES OFF)
			set(BUILD_STATIC ON)
			add_subdirectory("${DEP_DIR}")

			# dep/qtadvanceddocking/src/CMakeLists.txt pins its target to
			# CXX_STANDARD 17 for Qt6. Qt's inline containers carry
			# `#if __cplusplus >= 202002L` branches (QCommonArrayOps::
			# appendIteratorRange, for one), which is the same ODR hazard as a
			# Qt-version skew, so hold ADS to the project standard.
			set_target_properties(qtadvanceddocking-qt6 PROPERTIES
				CXX_STANDARD ${CMAKE_CXX_STANDARD}
				CXX_STANDARD_REQUIRED ON
			)

			set(NEURUS_DEP_${name}_FROM_LIB FALSE PARENT_SCOPE)
		endif()

	# -----------------------------------------------------------------------
	# Unknown dependency
	# -----------------------------------------------------------------------
	else()
		message(FATAL_ERROR "neurus_find_dependency: unknown dependency '${name}'. "
		                    "Supported: shaderc, qtadvanceddocking.")
	endif()

endfunction()
