# MiniRedis build script for Windows.
#
# Uses CMake when it is available, and otherwise falls back to invoking g++
# directly, so the project builds on a bare MSYS2 / MinGW-w64 install with no
# extra tooling.
#
#   .\scripts\build.ps1                # release build
#   .\scripts\build.ps1 -Config Debug  # debug build with sanitizer-friendly flags
#   .\scripts\build.ps1 -NoCMake       # force the direct g++ path

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [switch]$NoCMake
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

function Write-Step($message) {
    Write-Host "==> $message" -ForegroundColor Cyan
}

try {
    $binDir = Join-Path $root 'bin'
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null

    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake -and -not $NoCMake) {
        Write-Step "Configuring with CMake ($Config)"
        cmake -S . -B build -DCMAKE_BUILD_TYPE=$Config
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

        Write-Step "Building"
        cmake --build build --config $Config --parallel
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

        Copy-Item (Join-Path $root 'build/bin/*') $binDir -Force -ErrorAction SilentlyContinue
        Write-Host "Binaries in $binDir" -ForegroundColor Green
        exit 0
    }

    Write-Step "CMake not found (or -NoCMake given); compiling with g++ directly"

    $gpp = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $gpp) {
        throw "g++ was not found on PATH. Install MSYS2 (pacman -S mingw-w64-ucrt-x86_64-gcc) or add it to PATH."
    }

    if ($Config -eq 'Debug') {
        $flags = @('-std=c++20', '-g', '-O0', '-Wall', '-Wextra', '-Wno-unknown-pragmas', '-DMINIREDIS_DEBUG')
    } else {
        $flags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unknown-pragmas', '-DNDEBUG')
    }

    $coreSources = Get-ChildItem -Path (Join-Path $root 'src') -Filter '*.cpp' |
        Where-Object { $_.Name -notlike 'main_*' } |
        ForEach-Object { $_.FullName }

    $targets = @(
        @{ Name = 'miniredis-server';    Entry = 'src/main_server.cpp' },
        @{ Name = 'miniredis-cli';       Entry = 'src/main_cli.cpp' },
        @{ Name = 'miniredis-benchmark'; Entry = 'tools/benchmark.cpp' }
    )

    foreach ($target in $targets) {
        $entry = Join-Path $root $target.Entry
        if (-not (Test-Path $entry)) {
            Write-Host "  skipping $($target.Name) (no $($target.Entry))" -ForegroundColor DarkGray
            continue
        }
        Write-Step "Compiling $($target.Name)"
        $output = Join-Path $binDir "$($target.Name).exe"
        & g++ @flags -Iinclude $entry @coreSources -lws2_32 -o $output
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile $($target.Name)" }
    }

    $testSources = @(Get-ChildItem -Path (Join-Path $root 'tests') -Filter '*.cpp' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName })
    if ($testSources.Count -gt 0) {
        Write-Step "Compiling miniredis-tests"
        & g++ @flags -Iinclude -Itests @testSources @coreSources -lws2_32 -o (Join-Path $binDir 'miniredis-tests.exe')
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile the test suite" }
    }

    Write-Host "Build succeeded. Binaries in $binDir" -ForegroundColor Green
}
finally {
    Pop-Location
}
