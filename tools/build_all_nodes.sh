#!/usr/bin/env bash
# build_all_nodes.sh - Build firmware .hex/.bin for all 3 cluster nodes
# Must run inside nix develop (needs arm-none-eabi-gcc)
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build

for NID in 1 2 3; do
    echo "=== Building node $NID ==="
    rm -rf build/firmware
    cmake --preset firmware -DNODE_ID="$NID" 2>&1 | tail -1
    cmake --build build/firmware 2>&1 | tail -3
    cp "build/firmware/firmware/raft_oracle_node${NID}.hex" "build/raft_oracle_node${NID}.hex"
    cp "build/firmware/firmware/raft_oracle_node${NID}.bin" "build/raft_oracle_node${NID}.bin"
    echo "  -> build/raft_oracle_node${NID}.hex"
    echo ""
done

echo "=== All nodes built ==="
ls -la build/raft_oracle_node*.hex build/raft_oracle_node*.bin
echo ""
echo "Flash with: openocd -f board/st_nucleo_f2.cfg -c 'program build/raft_oracle_nodeN.hex verify reset exit'"
