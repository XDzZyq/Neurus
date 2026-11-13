#!/bin/sh
# Linux headless toolchain environment - source this file, do not execute it.
#
#   . scripts/linux/Toolchain.sh [TOOLCHAIN_ROOT]
#
# Puts a locally provisioned CMake, Ninja and Vulkan SDK on PATH without
# touching the system installation (no root required). The root defaults to
# $NEURUS_TOOLCHAIN, then $HOME/.local/neurus-toolchain.
#
# Expected layout under the root:
#   pyenv/bin/{cmake,ninja}        - CMake and Ninja (PyPI wheels or a venv)
#   vulkansdk/x86_64/{include,lib}  - LunarG Vulkan SDK extraction
#
# After sourcing, `cmake --version`, `ninja --version` and `vulkaninfo --summary`
# must all work before `make update` is run.

_neurus_toolchain_root="${1:-${NEURUS_TOOLCHAIN:-$HOME/.local/neurus-toolchain}}"

if [ ! -d "$_neurus_toolchain_root" ]; then
	echo "[Toolchain] No toolchain at $_neurus_toolchain_root" >&2
	echo "[Toolchain] Pass the root as an argument or set NEURUS_TOOLCHAIN." >&2
	return 1 2>/dev/null || exit 1
fi

if [ -d "$_neurus_toolchain_root/pyenv/bin" ]; then
	PATH="$_neurus_toolchain_root/pyenv/bin:$PATH"
fi

if [ -d "$_neurus_toolchain_root/vulkansdk/x86_64" ]; then
	VULKAN_SDK="$_neurus_toolchain_root/vulkansdk/x86_64"
	export VULKAN_SDK
	PATH="$VULKAN_SDK/bin:$PATH"
	# The SDK ships its own 1.4 loader, which the codebase requires.
	LD_LIBRARY_PATH="$VULKAN_SDK/lib/VulkanLoader/lib:$VULKAN_SDK/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
	export LD_LIBRARY_PATH
	VK_ADD_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
	export VK_ADD_LAYER_PATH
fi

NEURUS_TOOLCHAIN="$_neurus_toolchain_root"
export NEURUS_TOOLCHAIN

unset _neurus_toolchain_root
