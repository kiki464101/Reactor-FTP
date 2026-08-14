#!/bin/bash
# ================================================================
#  Rebuild script — LVGL FTP Client + Server
#  Run inside your Ubuntu VM:  bash rebuild.sh
# ================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo " 1/4  Installing build prerequisites"
echo "============================================"
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential cmake libsdl2-dev pkg-config 2>/dev/null || true

echo ""
echo "============================================"
echo " 2/4  Building CLIENT (LVGL + SDL)"
echo "============================================"
rm -rf build/CMakeCache.txt build/CMakeFiles
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
cd "$SCRIPT_DIR"

echo ""
echo "============================================"
echo " 3/4  Building SERVER"
echo "============================================"
cd server
rm -rf build/CMakeCache.txt build/CMakeFiles
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
cd "$SCRIPT_DIR"

echo ""
echo "============================================"
echo " 4/4  Build complete!"
echo ""
echo "  To run:"
echo ""
echo "    # Terminal 1 — start the server FIRST:"
echo "    cd $(pwd)"
echo "    ./bin/ftp_server 0.0.0.0 8888"
echo ""
echo "    # Terminal 2 — start the LVGL client:"
echo "    cd $(pwd)"
echo "    ./bin/main"
echo ""
echo "  Login defaults:  admin / 123456"
echo "============================================"
