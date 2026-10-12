# ---------------------------------------------------------------------------
# Neurus - Convenience Build Wrapper
#
# This Makefile provides quick-build shortcuts. All real logic is in CMake.
# Requires: cmake >= 3.27
#   Windows: Visual Studio 2022 with C++20 toolchain
#   macOS:   Ninja, AppleClang, Homebrew Qt6 + Vulkan SDK
# ---------------------------------------------------------------------------

# --- Platform detection ---
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  PRESET        := macos
  PRESET_REL    := macos
  PYTHON        := python3
  SETUP_PY_ENV  := sh lib/scripts/setup_python_env.sh
else
  PRESET        := win
  PRESET_REL    := win
  PYTHON        := python
  SETUP_PY_ENV  := powershell -ExecutionPolicy Bypass -File lib/scripts/setup_python_env.ps1
endif

.PHONY: configure build test clean nobuild release help app check update

# --- Debug (default) ---

configure:
	cmake --preset $(PRESET)

# Full build only when no sub-target (app/test) is specified.
#   make build       → full build
#   make build app   → Neurus.exe only
#   make build test  → neurus_test only
build:
ifeq ($(filter app test,$(MAKECMDGOALS)),)
	cmake --build build --config Debug
endif

# Build Neurus.exe only (make app  or  make build app)
app:
	cmake --build build --target Neurus --config Debug

# Build neurus_test only (make test  or  make build test)
test:
	cmake --build build --target neurus_test --config Debug

# Run tests. Pass FILTER for specific tests.
#   make check                          → all tests
#   make check FILTER="-R DeferredShading" → filtered
#
# There are two build trees and they are NOT interchangeable:
#   build/       multi-config (cmake --preset win|macos)
#   build/debug  single-config Ninja (the tree local work actually builds)
# On macOS the filesystem is case-insensitive, so build/Debug and build/debug
# are the same directory and only the single-config cache survives in it — but
# build/CTestTestfile.cmake is a separate, older file. Running `ctest -C Debug`
# from build/ therefore reports whatever that stale file last saw (841 tests)
# instead of the live suite (959), which is a green that silently skipped 118.
# Prefer build/debug whenever it exists; Windows has no such alias, so it falls
# back to the multi-config tree.
CHECK_DIR := $(if $(wildcard build/debug/CMakeCache.txt),build/debug,build)
CHECK_CONFIG := $(if $(filter build/debug,$(CHECK_DIR)),,-C Debug)

check:
	cd $(CHECK_DIR) && ctest $(CHECK_CONFIG) --output-on-failure $(FILTER)

clean:
	cmake --build build --target clean

# --- Release ---

release:
	cmake --preset $(PRESET_REL) && cmake --build build --config Release

# --- Visual Studio 2022 (outside source tree) ---

nobuild:
	cmake --preset vs2022
	@echo ""
	@echo "  Visual Studio solution generated at ../Neurus_VS2022/Neurus.sln"
	@echo "  Open it in VS 2022, right-click Neurus -> Set as Startup Project, then F5."
	@echo ""

# --- Help ---

help:
	@echo "Neurus Build System"
	@echo "==================="
	@echo ""
	@echo "  make update         - Init submodules, download pre-compiled deps via lib/, configure CMake"
	@echo "  make configure      - Configure Debug build (VS 2022)"
	@echo "  make build          - Build everything (Debug)"
	@echo "  make build app      - Build Neurus.exe only"
	@echo "  make build test     - Build neurus_test only"
	@echo "  make app            - Alias: build Neurus.exe only"
	@echo "  make test           - Alias: build neurus_test only"
	@echo "  make check          - Run all tests"
	@echo "  make check FILTER=\"-R Pattern\" - Run specific tests"
	@echo "  make clean          - Clean Debug build"
	@echo "  make release        - Configure + Build Release"
	@echo "  make nobuild        - Generate VS 2022 solution at ../Neurus_VS2022"

# --- Dependency Setup ---

update:
	git submodule update --init --recursive --depth 1
	# googletest is `update = none` (lazy) in lib/.gitmodules; init it on demand.
	git -C lib -c "submodule.src/googletest.update=checkout" submodule update --init --depth 1 src/googletest
	$(PYTHON) lib/scripts/fetch_prebuilt.py
	$(SETUP_PY_ENV)
	cmake --preset $(PRESET) $(if $(BINARY_DIR),-B $(BINARY_DIR),)
