param(
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$CommonFlags = @(
    "-std=c++23",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-DUNICODE",
    "-D_UNICODE"
)

if (-not $SkipTests) {
    Write-Host "[1/2] Compiling and running unit tests..."
    New-Item -ItemType Directory -Force -Path "build" | Out-Null
    $TestExe = Join-Path $PSScriptRoot "build\test_core.exe"
    & g++ @CommonFlags -static `
        "tests/test_core.cpp" `
        "src/model.cpp" `
        "src/storage.cpp" `
        -o $TestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Unit test compilation failed."
    }

    & $TestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Unit tests failed."
    }
    Remove-Item -Force $TestExe -ErrorAction SilentlyContinue
    Remove-Item -Force "build" -ErrorAction SilentlyContinue
}

Write-Host "[2/2] Building TodoFloat.exe (Release, static C++ runtime, stripped)..."
$OutExe = Join-Path $PSScriptRoot "TodoFloat.exe"
& g++ @CommonFlags -s -municode -mwindows -static `
    "src/main.cpp" `
    "src/model.cpp" `
    "src/storage.cpp" `
    "src/ui_window.cpp" `
    -ld2d1 -ldwrite -ldwmapi -lole32 -luuid -limm32 -lshell32 -luser32 -lgdi32 `
    -o $OutExe
if ($LASTEXITCODE -ne 0) {
    throw "Main executable build failed."
}

$FileInfo = Get-Item $OutExe
$SizeKB = [math]::Round($FileInfo.Length / 1KB, 1)
Write-Host "[OK] Built $OutExe ($SizeKB KB)"
