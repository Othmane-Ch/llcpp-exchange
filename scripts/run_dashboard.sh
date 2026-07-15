#!/usr/bin/env bash
# Launch the Streamlit metrics dashboard for the llcpp exchange.
# Usage: ./scripts/run_dashboard.sh [--port PORT]
#
# Options:
#   --port PORT   Port to serve on (default: 8501)

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ── Parse arguments ───────────────────────────────────────────────────────
PORT=8501
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--port PORT]"
            echo "  --port PORT   Port to serve on (default: 8501)"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Check Python dependencies ─────────────────────────────────────────────
if ! python3 -c "import streamlit" 2>/dev/null; then
    error "streamlit is not installed."
    echo "  Install it with:  pip install streamlit"
    echo "  Or:               sudo apt-get install python3-pip && pip install streamlit"
    exit 1
fi

# ── Build the Python module if needed ──────────────────────────────────────
if ! ls "${BUILD_DIR}"/bindings/python/exchange_core*.so 1>/dev/null 2>&1; then
    info "Python module not found. Building..."
    cmake -B "${BUILD_DIR}" -S "${PROJECT_ROOT}" \
        -DBUILD_PYTHON_BINDINGS=ON \
        -DENABLE_PERF_TRACKING=ON \
        2>&1 | tail -3
    cmake --build "${BUILD_DIR}" --target exchange_core -j"$(nproc)" 2>&1 | tail -5
    echo
fi

# Verify the module imports correctly
if ! python3 -c "
import sys; sys.path.insert(0, '${BUILD_DIR}/bindings/python')
import exchange_core
" 2>/dev/null; then
    error "exchange_core module failed to import. Try rebuilding:"
    echo "  cmake -B build -DBUILD_PYTHON_BINDINGS=ON -DENABLE_PERF_TRACKING=ON"
    echo "  cmake --build build --target exchange_core -j\$(nproc)"
    exit 1
fi

# ── Ensure Streamlit config for headless mode ─────────────────────────────
STREAMLIT_CONFIG_DIR="${PROJECT_ROOT}/.streamlit"
STREAMLIT_CONFIG="${STREAMLIT_CONFIG_DIR}/config.toml"
if [[ ! -f "${STREAMLIT_CONFIG}" ]]; then
    mkdir -p "${STREAMLIT_CONFIG_DIR}"
    cat > "${STREAMLIT_CONFIG}" <<'TOML'
[server]
headless = true

[browser]
gatherUsageStats = false
TOML
    info "Created .streamlit/config.toml"
fi

# ── Launch Streamlit ───────────────────────────────────────────────────────
info "Starting Streamlit dashboard on port ${PORT}..."
info "Open http://localhost:${PORT} in your browser."
echo

cd "${PROJECT_ROOT}"
exec streamlit run dashboard/app.py --server.port="${PORT}"
