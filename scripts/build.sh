#!/usr/bin/env bash
# Build the llcpp exchange project.
# Usage: ./scripts/build.sh [--python] [--perf] [--clean]
#
# Options:
#   --python    Build pybind11 Python bindings (exchange_core module)
#   --perf      Enable hot-path latency instrumentation (LatencyTracker)
#   --clean     Remove build directory and rebuild from scratch

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

GREEN='\033[0;32m'
NC='\033[0m'
info() { echo -e "${GREEN}[INFO]${NC}  $*"; }

# ── Parse flags ────────────────────────────────────────────────────────────
BUILD_PYTHON=OFF
ENABLE_PERF=OFF
CLEAN=false

for arg in "$@"; do
    case "$arg" in
        --python) BUILD_PYTHON=ON ;;
        --perf)   ENABLE_PERF=ON ;;
        --clean)  CLEAN=true ;;
        --help|-h)
            echo "Usage: $0 [--python] [--perf] [--clean]"
            echo "  --python  Build Python bindings"
            echo "  --perf    Enable LatencyTracker instrumentation"
            echo "  --clean   Clean rebuild"
            exit 0
            ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# ── Clean if requested ────────────────────────────────────────────────────
if $CLEAN && [[ -d "${BUILD_DIR}" ]]; then
    info "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# ── Configure ──────────────────────────────────────────────────────────────
info "Configuring (PYTHON=${BUILD_PYTHON}, PERF=${ENABLE_PERF})..."
cmake -B "${BUILD_DIR}" -S "${PROJECT_ROOT}" \
    -DBUILD_PYTHON_BINDINGS="${BUILD_PYTHON}" \
    -DENABLE_PERF_TRACKING="${ENABLE_PERF}" \
    2>&1 | tail -5
echo

# ── Build ──────────────────────────────────────────────────────────────────
info "Building with $(nproc) threads..."
cmake --build "${BUILD_DIR}" -j"$(nproc)" 2>&1
echo

info "Build complete."
echo "  Exchange:    ${BUILD_DIR}/Exchange/exchange_main"
echo "  C++ tests:   ${BUILD_DIR}/Exchange/me_order_book_tests"
if [[ "${BUILD_PYTHON}" == "ON" ]]; then
    echo "  Python module: ${BUILD_DIR}/bindings/python/exchange_core*.so"
fi
