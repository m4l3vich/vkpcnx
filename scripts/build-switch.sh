#!/usr/bin/env bash
# Builds the Switch .nro. Needs devkitPro with the switch-dev group plus
# switch-curl switch-zlib switch-libopus switch-ffmpeg switch-libwebp switch-glfw switch-mesa
# switch-libdrm_nouveau (all in the devkitpro/devkita64 Docker image).
#
#   ./scripts/build-switch.sh            # native devkitPro install ($DEVKITPRO)
#   ./scripts/build-switch.sh --docker   # inside devkitpro/devkita64:latest
#   ./scripts/build-switch.sh --debug    # Debug build in build-switch-debug/ with nxlink stdio
#                                        # (flags combine: --docker --debug)
set -euo pipefail
cd "$(dirname "$0")/.."

DOCKER=0
BUILD_TYPE=Release
BUILD_DIR=build-switch
EXTRA_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --docker) DOCKER=1 ;;
        --debug) BUILD_TYPE=Debug; BUILD_DIR=build-switch-debug; EXTRA_ARGS+=(--debug) ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [[ $DOCKER == 1 ]]; then
    TTY_FLAGS=(-i)
    [[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
    # The container's git refuses the host-owned checkout, so the version
    # stamp (cmake/GitVersion.cmake) is taken here
    DESCRIBE="$(git describe --always --dirty --abbrev=12 --exclude='*' 2>/dev/null || true)"
    exec docker run --rm "${TTY_FLAGS[@]}" -v "$PWD:/work" -w /work \
        -e VKPCNX_JOBS="${VKPCNX_JOBS:-}" -e VKPCNX_GIT_DESCRIBE="$DESCRIBE" \
        devkitpro/devkita64:latest ./scripts/build-switch.sh ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
fi

: "${DEVKITPRO:?set DEVKITPRO to your devkitPro root}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"
JOBS="${VKPCNX_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# borealis' switch_wrapper.c only calls nxlinkStdio() when DEBUG is defined.
DEBUG_FLAGS=()
[[ $BUILD_TYPE == Debug ]] && DEBUG_FLAGS=(-DCMAKE_C_FLAGS_DEBUG="-g -O0 -DDEBUG" -DCMAKE_CXX_FLAGS_DEBUG="-g -O0 -DDEBUG")

cmake -B "$BUILD_DIR" -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ${DEBUG_FLAGS[@]+"${DEBUG_FLAGS[@]}"}
cmake --build "$BUILD_DIR" -j"$JOBS" --target vkpcnx.nro
