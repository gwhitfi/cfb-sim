#!/usr/bin/env bash
# Rebuilds the server, stops whatever is on the server's and the viewer's
# ports, and starts both. Ctrl+C stops both; run it again to restart.
#
#   ./scripts/dev.sh
#
# Ports: 8765 (cfbsim-server), 5173 (the viewer's dev server).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --build --preset release
fuser -k 8765/tcp 5173/tcp 2>/dev/null || true
sleep 0.5

trap 'kill 0' EXIT
./build/release/server/cfbsim-server &
cd viewer && npm run dev
