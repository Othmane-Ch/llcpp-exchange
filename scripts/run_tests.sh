#!/usr/bin/env bash
# Run all tests (C++, Python, and BDD) for the llcpp exchange.
# Usage: ./scripts/run_tests.sh

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

FAILURES=0

# ── Build if needed ────────────────────────────────────────────────────────
if [[ ! -f "${BUILD_DIR}/Exchange/me_order_book_tests" ]] || \
   ! ls "${BUILD_DIR}"/bindings/python/exchange_core*.so 1>/dev/null 2>&1; then
    info "Building project with Python bindings and perf tracking..."
    cmake -B "${BUILD_DIR}" -S "${PROJECT_ROOT}" \
        -DBUILD_PYTHON_BINDINGS=ON \
        -DENABLE_PERF_TRACKING=ON \
        2>&1 | tail -3
    cmake --build "${BUILD_DIR}" -j"$(nproc)" 2>&1 | tail -5
    echo
fi

# ── C++ tests ──────────────────────────────────────────────────────────────
info "Running C++ tests (doctest)..."
echo "─────────────────────────────────────────"
if "${BUILD_DIR}/Exchange/me_order_book_tests" 2>/dev/null; then
    info "C++ tests passed."
else
    error "C++ tests failed."
    FAILURES=$((FAILURES + 1))
fi
echo

# ── Python tests ───────────────────────────────────────────────────────────
info "Running Python tests (pytest)..."
echo "─────────────────────────────────────────"
cd "${PROJECT_ROOT}"
if python3 -m pytest tests/ -v --tb=short 2>&1; then
    info "Python tests passed."
else
    error "Python tests failed."
    FAILURES=$((FAILURES + 1))
fi
echo

# ── BDD tests ─────────────────────────────────────────────────────────────
if command -v behave &>/dev/null && [[ -d "${PROJECT_ROOT}/features" ]]; then
    info "Running BDD tests (behave)..."
    echo "─────────────────────────────────────────"
    cd "${PROJECT_ROOT}"
    if behave --format progress --no-capture 2>&1; then
        info "BDD tests passed."
    else
        error "BDD tests failed."
        FAILURES=$((FAILURES + 1))
    fi
    echo
else
    warn "Skipping BDD tests — behave not installed or features/ directory missing."
    warn "Install with: pip install behave"
    echo
fi

# ── Summary ────────────────────────────────────────────────────────────────
if [[ ${FAILURES} -eq 0 ]]; then
    info "All tests completed successfully."
else
    error "${FAILURES} test suite(s) failed."
    exit 1
fi
