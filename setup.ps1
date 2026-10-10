# Neurus - Environment Setup Script
#
# Run this script after installing the prerequisites:
#   1. Vulkan SDK 1.4.x from https://vulkan.lunarg.com/ (install to D:\VulkanSDK)
#   2. Qt 6.8+ from https://www.qt.io/download (install to D:\Qt)
#
# Usage: .\setup.ps1

param(
    [string]$VulkanSdkPath = "D:\VulkanSDK\1.4.341.0",
    [string]$QtPath = "D:\Qt\6.11.1\msvc2022_64"
)

Write-Host "=== Neurus Environment Setup ===" -ForegroundColor Cyan

# --- Vulkan SDK ---
if (Test-Path "$VulkanSdkPath\Bin\glslangValidator.exe") {
    Write-Host "[OK] Vulkan SDK found at $VulkanSdkPath" -ForegroundColor Green
    $env:VULKAN_SDK = $VulkanSdkPath
    [Environment]::SetEnvironmentVariable("VULKAN_SDK", $VulkanSdkPath, "User")
    Write-Host "  VULKAN_SDK set to $VulkanSdkPath"
}
else {
    Write-Host "[MISSING] Vulkan SDK not found at $VulkanSdkPath" -ForegroundColor Yellow
    Write-Host "  Download from: https://vulkan.lunarg.com/"
}

# --- Qt ---
if (Test-Path "$QtPath\bin\qmake.exe") {
    Write-Host "[OK] Qt found at $QtPath" -ForegroundColor Green
    $env:CMAKE_PREFIX_PATH = $QtPath
    [Environment]::SetEnvironmentVariable("CMAKE_PREFIX_PATH", $QtPath, "User")
    Write-Host "  CMAKE_PREFIX_PATH set to $QtPath"
}
else {
    Write-Host "[MISSING] Qt not found at $QtPath" -ForegroundColor Yellow
    Write-Host "  Download from: https://www.qt.io/download"
    Write-Host "  Or install via aqt: pip install aqtinstall && aqt install-qt windows desktop 6.8.0 win64_msvc2022_64"
}

# --- Build ---
Write-Host ""
Write-Host "=== Build ===" -ForegroundColor Cyan

if ($env:VULKAN_SDK -and $env:CMAKE_PREFIX_PATH) {
    Write-Host "[READY] All prerequisites found. Building..." -ForegroundColor Green
    Write-Host ""

    # Clean previous build
    if (Test-Path "build") { Remove-Item -Recurse -Force "build" }

    # Configure
    cmake --preset win
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] CMake configuration failed." -ForegroundColor Red
        exit 1
    }

    # Build
    cmake --build build/debug --config Debug
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Build failed." -ForegroundColor Red
        exit 1
    }

    Write-Host ""
    Write-Host "=== Build Successful ===" -ForegroundColor Green
    Write-Host "  Run: .\build\debug\Debug\Neurus.exe"
    Write-Host "  Test: cd build\debug; ctest -C Debug --output-on-failure"
}
else {
    Write-Host "[NOT READY] Install the missing prerequisites above, then re-run this script." -ForegroundColor Yellow
}
