#!/usr/bin/env bash
# MiniRedis build script for Linux and macOS.
#
# Prefers CMake, falls back to a direct g++/clang++ invocation.
#
#   ./scripts/build.sh              # release build
#   ./scripts/build.sh debug        # debug build
#   MINIREDIS_NO_CMAKE=1 ./scripts/build.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

config="${1:-release}"
case "$config" in
    release|Release) build_type=Release ;;
    debug|Debug)     build_type=Debug ;;
    *) echo "usage: $0 [release|debug]" >&2; exit 2 ;;
esac

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }

mkdir -p bin

if command -v cmake >/dev/null 2>&1 && [ -z "${MINIREDIS_NO_CMAKE:-}" ]; then
    step "Configuring with CMake ($build_type)"
    cmake -S . -B build -DCMAKE_BUILD_TYPE="$build_type"

    step "Building"
    cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

    cp -f build/bin/* bin/ 2>/dev/null || true
    printf '\033[32mBinaries in %s/bin\033[0m\n' "$root"
    exit 0
fi

step "CMake not found; compiling directly"

CXX="${CXX:-g++}"
if [ "$build_type" = "Debug" ]; then
    flags=(-std=c++20 -g -O0 -Wall -Wextra -DMINIREDIS_DEBUG)
else
    flags=(-std=c++20 -O2 -Wall -Wextra -DNDEBUG)
fi

mapfile -t core_sources < <(find src -maxdepth 1 -name '*.cpp' ! -name 'main_*' | sort)

build_target() {
    local name="$1" entry="$2"
    if [ ! -f "$entry" ]; then
        printf '  skipping %s (no %s)\n' "$name" "$entry"
        return
    fi
    step "Compiling $name"
    "$CXX" "${flags[@]}" -Iinclude "$entry" "${core_sources[@]}" -lpthread -o "bin/$name"
}

build_target miniredis-server    src/main_server.cpp
build_target miniredis-cli       src/main_cli.cpp
build_target miniredis-benchmark tools/benchmark.cpp

if compgen -G "tests/*.cpp" >/dev/null; then
    step "Compiling miniredis-tests"
    mapfile -t test_sources < <(find tests -maxdepth 1 -name '*.cpp' | sort)
    "$CXX" "${flags[@]}" -Iinclude -Itests "${test_sources[@]}" "${core_sources[@]}" -lpthread -o bin/miniredis-tests
fi

printf '\033[32mBuild succeeded. Binaries in %s/bin\033[0m\n' "$root"
