# Builds mxb_physics.dll (x64, Release) into .\build\Release\
#
# Needs: Visual Studio 2022 with the C++ workload, CMake, and MinHook via vcpkg:
#   vcpkg install minhook:x64-windows-static
# Point $VcpkgRoot at your vcpkg checkout if it is not in the default place.

param(
    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg",
    [string]$Config    = "Release"
)

$ErrorActionPreference = "Stop"
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain not found at $toolchain - pass -VcpkgRoot <path>"
}

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
      "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
cmake --build build --config $Config

Write-Host ""
Write-Host "Built: build\$Config\mxb_physics.dll"
