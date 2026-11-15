# Rebuilds and, if the game is holding the old DLL, says so instead of failing
# with an opaque linker error.
#
# Pressing END in game unloads the module cleanly and releases the file.

param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$game = Get-Process mxbikes -ErrorAction SilentlyContinue
if ($game) {
    Write-Host "mxbikes.exe is running (PID $($game.Id))." -ForegroundColor Yellow
    Write-Host "Press END in game to unload the module, or close the game, then run this again."
    Write-Host "Building anyway in case the module is already unloaded..."
}

cmake --build build --config $Config
Write-Host ""
Write-Host "Built: build\$Config\mxb_physics.dll"
