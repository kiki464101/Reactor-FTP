#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

sudo apt-get install -y -qq build-essential cmake 2>/dev/null || true
mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)
echo ""
echo "============================================"
echo "  Server build complete!"
echo "  Run:  ../bin/ftp_server 0.0.0.0 8888"
echo "============================================"
