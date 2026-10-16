#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"
git submodule update --init --recursive Util/SimRobot Util/CompiledNN
Make/Common/pullONNXRuntime
if [[ -z "${RCSS_CXX_COMPILER:-}" && ! -x "$REPO_DIR/.rcss-toolchain/bin/clang++" && -x "$REPO_DIR/.hsl-toolchain/bin/clang++" ]]; then
  RCSS_CXX_COMPILER="$REPO_DIR/.hsl-toolchain/bin/clang++"
fi
RCSS_CXX_COMPILER="${RCSS_CXX_COMPILER:-$REPO_DIR/.rcss-toolchain/bin/clang++}"
if [[ ! -x "$RCSS_CXX_COMPILER" ]]; then
  echo 'Run python3 scripts/setup_sim_toolchain.py first, or set RCSS_CXX_COMPILER to Clang >= 16.' >&2
  exit 1
fi
if [[ -f build_sim/CMakeCache.txt ]]; then
  CACHED_COMPILER="$(sed -n 's/^set(CMAKE_CXX_COMPILER "\(.*\)")/\1/p' build_sim/CMakeFiles/*/CMakeCXXCompiler.cmake 2>/dev/null || true)"
  if [[ -n "$CACHED_COMPILER" && "$CACHED_COMPILER" != "$RCSS_CXX_COMPILER" ]]; then
    cmake -E rm -f build_sim/CMakeCache.txt
    cmake -E rm -rf build_sim/CMakeFiles
  fi
fi
cmake -S Make/CMake -B build_sim -DBUILD_RCSS_SERVER=ON -DCMAKE_BUILD_TYPE=Release \
  -DRCSS_CXX_COMPILER="$RCSS_CXX_COMPILER" "$@"
cmake --build build_sim --target run_player -j"${RCSS_BUILD_JOBS:-4}"
