#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

cleanup() {
  if [[ -n "${TRACKER_PID:-}" ]]; then kill "$TRACKER_PID" 2>/dev/null || true; fi
  if [[ -n "${PEER_PID:-}" ]]; then kill "$PEER_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

make clean >/dev/null
make >/dev/null

rm -rf demo_tmp
mkdir -p demo_tmp/peer1
printf 'hello from demo\n' > demo_tmp/peer1/demo.txt

./tracker tracker.conf > demo_tmp/tracker.log 2>&1 &
TRACKER_PID=$!

./peer serve demo_tmp/peer1 5001 > demo_tmp/peer.log 2>&1 &
PEER_PID=$!

sleep 1

echo "[1/5] create tracker"
./peer peer.conf createtracker demo_tmp/peer1/demo.txt demo_file 127.0.0.1 5001

echo "[2/5] list"
./peer peer.conf list

echo "[3/5] get tracker metadata"
./peer peer.conf get demo.txt demo_tmp/demo.track

echo "[4/5] download"
./peer download demo_tmp/demo.track demo_tmp/downloaded_demo.txt

echo "[5/5] verify"
cmp -s demo_tmp/peer1/demo.txt demo_tmp/downloaded_demo.txt

echo "Demo completed successfully."
