#!/usr/bin/env bash
# Configure, build and test The Dojo on Linux using the CMake presets.
#
#   ./scripts/build.sh                 # GCC, Release
#   ./scripts/build.sh debug           # GCC, Debug (RT allocation trap enabled in the app)
#   ./scripts/build.sh release --clang
#   ./scripts/build.sh debug --run     # ...and launch the app afterwards
#   ./scripts/build.sh --no-tests
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

config="release"
compiler="gcc"
run_tests=1
run_app=0

for arg in "$@"; do
    case "$arg" in
        debug|release) config="$arg" ;;
        --clang) compiler="clang" ;;
        --gcc) compiler="gcc" ;;
        --no-tests) run_tests=0 ;;
        --run) run_app=1 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if [[ "$compiler" == "clang" ]]; then
    preset="linux-clang-${config}"
else
    preset="linux-${config}"
fi

echo "==> Preset: ${preset}"
cmake --preset "${preset}"
cmake --build --preset "${preset}" --parallel

if [[ $run_tests -eq 1 ]]; then
    ctest --preset "${preset}"
fi

cmake_config="$( [[ "$config" == "debug" ]] && echo Debug || echo Release )"
app="build/${preset}/src/app/TheDojo_artefacts/${cmake_config}/TheDojo"
echo
echo "==> Built: ${app}"

if [[ $run_app -eq 1 ]]; then
    exec "${app}"
fi
