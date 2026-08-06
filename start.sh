#!/bin/bash
# =============================================================================
# HiveMind Node Graph — startup / build / lint script
#
# Options:
#   --build       configure + build (implies --no-start)
#   --clang       build with clang instead of GCC
#   --lint        run clang-tidy then exit (requires build with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
#   --lint-fix    run clang-tidy --fix then exit
#   --clean       delete build directory before building
#   --no-start    configure and build but do not start the server
#   --port N      override the server port (default: 8421)
#
# Examples:
#   ./start.sh                          # start with existing build
#   ./start.sh --build                  # configure + build + start
#   ./start.sh --clang --build          # configure + build with clang + start
#   ./start.sh --clang --build --clean  # fresh clang build + start
#   ./start.sh --lint                   # run clang-tidy analysis + exit
#   ./start.sh --lint-fix               # apply clang-tidy fixes + exit
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BINARY="${BUILD_DIR}/hivemind"

# ── Parse flags ────────────────────────────────────────────────────────────────
RUN_BUILD=false
RUN_LINT=false
RUN_LINT_FIX=false
RUN_CLEAN=false
RUN_START=true
USE_CLANG=false
CUSTOM_PORT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)    RUN_BUILD=true;  RUN_START=false ;;
        --clang)    USE_CLANG=true ;;
        --lint)     RUN_LINT=true  ;;
        --lint-fix) RUN_LINT_FIX=true ;;
        --clean)    RUN_CLEAN=true ;;
        --no-start) RUN_START=false; RUN_BUILD=true ;;
        --port)     CUSTOM_PORT="$2"; shift ;;
        --help|-h)
            sed -n '1,/^# Examples:/p' "$0" | head -n -1
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

# Resolve interpreter path across distributions
resolve_interpreter() {
    for p in /usr/lib64/ld-linux-x86-64.so.2 \
              /usr/lib/ld-linux-x86-64.so.2 \
              /lib64/ld-linux-x86-64.so.2; do
        if [[ -x "$p" ]]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

INTERP="$(resolve_interpreter)" || {
    echo "[start.sh] ERROR: cannot find dynamic linker (ld-linux-x86-64.so.2)" >&2
    exit 1
}

PORT="${CUSTOM_PORT:-${HIVEMIND_PORT:-8421}}"

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ "$RUN_BUILD" == true ]] || [[ "$RUN_CLEAN" == true ]]; then
    if [[ "$RUN_CLEAN" == true ]]; then
        echo "[start.sh] Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
    fi

    echo "[start.sh] Configuring CMake..."
    CMAKE_ARGS=(
        -S "${SCRIPT_DIR}"
        -B "${BUILD_DIR}"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON   # required for clang-tidy
    )

    if [[ "$USE_CLANG" == true ]]; then
        echo "[start.sh]   Compiler: clang / clang++"
        CMAKE_ARGS+=(
            -DCMAKE_C_COMPILER=clang
            -DCMAKE_CXX_COMPILER=clang++
        )
    fi

    cmake "${CMAKE_ARGS[@]}"

    echo "[start.sh] Building..."
    cmake --build "${BUILD_DIR}" -j"$(nproc)"

    echo "[start.sh] Build complete."
fi

# ── Lint ─────────────────────────────────────────────────────────────────────
if [[ "$RUN_LINT" == true ]]; then
    if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
        echo "[start.sh] ERROR: compile_commands.json not found." >&2
        echo "[start.sh] Re-run with --build to generate it." >&2
        exit 1
    fi
    echo "[start.sh] Running clang-tidy (dry run)..."
    if command -v clang-tidy &>/dev/null; then
        (cd "${BUILD_DIR}" && clang-tidy \
            --config-file="${SCRIPT_DIR}/.clang-tidy" \
            --export-fixes=lint-results.yaml \
            -p "${BUILD_DIR}" \
            ${SCRIPT_DIR}/src/main.cpp \
            ${SCRIPT_DIR}/src/Graph.cpp \
            ${SCRIPT_DIR}/src/GraphStore.cpp \
            ${SCRIPT_DIR}/src/NodeExecutor.cpp \
            ${SCRIPT_DIR}/src/ExecutorRegistry.cpp \
            ${SCRIPT_DIR}/src/LoopEngine.cpp \
            ${SCRIPT_DIR}/src/GraphRunner.cpp \
            ${SCRIPT_DIR}/src/Settings.cpp \
            ${SCRIPT_DIR}/src/executors/PromptExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/AgentExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/LoopStartExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/LoopEndExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/InputExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/OutputExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/LogExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/MergeExecutor.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/GraphRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/RunRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/StaticRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/ModelRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/ProjectRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/AgentRoutes.cpp \
            2>&1 | head -n 200)
        echo "[start.sh] clang-tidy complete. Results in ${BUILD_DIR}/lint-results.yaml"
    else
        echo "[start.sh] ERROR: clang-tidy not found." >&2
        echo "[start.sh] Install with:  apt install clang-tidy" >&2
        echo "[start.sh] daedalus also needs:  apt install libssl-dev libpng-dev libpoppler-cpp-dev" >&2
        exit 1
    fi
fi

if [[ "$RUN_LINT_FIX" == true ]]; then
    if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
        echo "[start.sh] ERROR: compile_commands.json not found." >&2
        echo "[start.sh] Re-run with --build to generate it." >&2
        exit 1
    fi
    echo "[start.sh] Running clang-tidy --fix..."
    if command -v clang-tidy &>/dev/null; then
        (cd "${BUILD_DIR}" && clang-tidy \
            --config-file="${SCRIPT_DIR}/.clang-tidy" \
            --fix \
            --fix-errors \
            -p "${BUILD_DIR}" \
            ${SCRIPT_DIR}/src/main.cpp \
            ${SCRIPT_DIR}/src/Graph.cpp \
            ${SCRIPT_DIR}/src/GraphStore.cpp \
            ${SCRIPT_DIR}/src/NodeExecutor.cpp \
            ${SCRIPT_DIR}/src/ExecutorRegistry.cpp \
            ${SCRIPT_DIR}/src/LoopEngine.cpp \
            ${SCRIPT_DIR}/src/GraphRunner.cpp \
            ${SCRIPT_DIR}/src/Settings.cpp \
            ${SCRIPT_DIR}/src/executors/PromptExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/AgentExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/LoopStartExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/LoopEndExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/InputExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/OutputExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/LogExecutor.cpp \
            ${SCRIPT_DIR}/src/executors/MergeExecutor.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/GraphRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/RunRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/StaticRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/ModelRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/ProjectRoutes.cpp \
            ${SCRIPT_DIR}/include/hivemind/routes/AgentRoutes.cpp \
            2>&1 | head -n 200)
        echo "[start.sh] clang-tidy --fix complete. Review changes with: git diff"
    else
        echo "[start.sh] ERROR: clang-tidy not found." >&2
        exit 1
    fi
fi

# ── Start ─────────────────────────────────────────────────────────────────────
if [[ "$RUN_START" != true ]]; then
    exit 0
fi

# Verify binary
