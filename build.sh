#!/bin/bash
# ================================================================
#  Build script for LVGL FTP Client (Ubuntu 24.04 / Linux)
# ================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "==> Installing build prerequisites..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    build-essential \
    cmake \
    libsdl2-dev \
    pkg-config \
    2>/dev/null || {
    echo "WARNING: apt install failed â€?continuing (may already be installed)."
}

echo "==> Configuring with CMake..."
mkdir -p build
cd build
cmake ..

echo "==> Building..."
cmake --build . -j$(nproc)

echo ""
echo "============================================"
echo "  Build complete!"
echo "  Run:  ./bin/main"
echo "============================================"

