#!/usr/bin/env bash
set -euo pipefail

# T10 Hardware Verification - Single-node Raft on Nucleo-F207ZG
#
# Prerequisites:
#   - Nucleo-F207ZG connected via USB (ST-LINK + VCP)
#   - Ethernet cable from Nucleo to laptop/switch
#   - Running inside: nix develop
#
# Usage:
#   ./tools/verify_t10.sh           # full sequence
#   ./tools/verify_t10.sh flash     # just flash
#   ./tools/verify_t10.sh uart      # just monitor UART
#   ./tools/verify_t10.sh sniff     # just sniff Ethernet
#   ./tools/verify_t10.sh heartbeat # send test heartbeats

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build/firmware"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }

# Auto-detect UART device (ST-LINK VCP)
find_uart() {
    # Nucleo ST-LINK VCP typically shows up as ttyACM0
    for dev in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0; do
        if [ -c "$dev" ]; then
            echo "$dev"
            return
        fi
    done
    # WSL: check for COM ports
    if [ -n "${WSL_DISTRO_NAME:-${WSL_DISTRO:-}}" ]; then
        warn "WSL detected - UART may need usbipd attach first"
        warn "  From PowerShell: usbipd.exe list"
        warn "  Then: usbipd.exe attach --wsl --busid <BUS_ID>"
    fi
    return 1
}

# Auto-detect Ethernet interface connected to STM32
find_eth_iface() {
    # Look for an interface with a link-local or no IP that's UP
    # Common names: eth0, enp0s*, enx* (USB-Ethernet)
    for iface in $(ip -br link show | awk '/UP/ {print $1}'); do
        # Skip loopback and wireless
        case "$iface" in lo|wl*|wlan*) continue ;; esac
        echo "$iface"
        return
    done
    return 1
}

cmd_build() {
    info "Building firmware..."
    cmake --preset firmware 2>/dev/null || cmake --preset firmware
    cmake --build "$BUILD_DIR" 2>&1
    ok "Build complete"
    echo ""
    info "Binary sizes:"
    ls -la "$BUILD_DIR/firmware/app.elf" "$BUILD_DIR/firmware/app.bin" "$BUILD_DIR/firmware/app.hex" 2>/dev/null
}

cmd_flash() {
    info "Flashing firmware via OpenOCD + ST-LINK..."
    if ! command -v openocd &>/dev/null; then
        fail "openocd not found - are you in nix develop?"
        exit 1
    fi

    # Check for ST-LINK
    if ! st-info --probe 2>/dev/null | grep -q "Found"; then
        fail "No ST-LINK detected. Is the board connected via USB?"
        exit 1
    fi

    cmake --build "$BUILD_DIR" --target flash 2>&1
    ok "Flash complete"
}

cmd_uart() {
    local uart_dev
    uart_dev=$(find_uart) || { fail "No UART device found"; exit 1; }

    info "Monitoring UART on $uart_dev @ 115200 baud"
    info "Press Ctrl-C to stop"
    echo "=========================================="

    # Use stty + cat for simple monitoring (no minicom dependency)
    stty -F "$uart_dev" 115200 cs8 -cstopb -parenb raw -echo
    cat "$uart_dev"
}

cmd_uart_timed() {
    # Capture UART output for a fixed duration, then analyze
    local duration="${1:-60}"
    local uart_dev
    uart_dev=$(find_uart) || { fail "No UART device found"; exit 1; }
    local logfile="/tmp/t10_uart_$(date +%Y%m%d_%H%M%S).log"

    info "Capturing UART for ${duration}s to $logfile"
    stty -F "$uart_dev" 115200 cs8 -cstopb -parenb raw -echo

    timeout "$duration" cat "$uart_dev" | tee "$logfile" || true

    echo ""
    echo "=========================================="
    info "Analyzing captured output..."
    echo ""

    # Check T10 verification criteria
    local pass=0
    local total=6

    # 1. Boot in <1s
    if grep -q "boot ok" "$logfile"; then
        ok "[1/6] Boot message found"
        ((pass++))
    else
        fail "[1/6] No boot message - board may not have booted"
    fi

    # 2. Raft state = LEADER
    if grep -q "state=LEADER" "$logfile"; then
        ok "[2/6] Raft state = LEADER"
        ((pass++))
    else
        fail "[2/6] Raft not in LEADER state"
    fi

    # 3. Heap within budget
    if grep -q "heap:" "$logfile"; then
        # Extract last heap free value
        local heap_free
        heap_free=$(grep "heap:" "$logfile" | tail -1 | grep -oP '\d+ free' | grep -oP '\d+')
        if [ -n "$heap_free" ] && [ "$heap_free" -ge 26000 ]; then
            ok "[3/6] Heap free: ${heap_free} bytes (>= 26KB)"
            ((pass++))
        else
            fail "[3/6] Heap free: ${heap_free:-unknown} bytes (need >= 26KB)"
        fi
    else
        fail "[3/6] No heap stats found"
    fi

    # 4. No stack overflows
    if grep -q "FATAL: stack overflow" "$logfile"; then
        fail "[4/6] Stack overflow detected!"
    else
        ok "[4/6] No stack overflows"
        ((pass++))
    fi

    # 5. No malloc failures
    if grep -q "FATAL: pvPortMalloc" "$logfile"; then
        fail "[5/6] Malloc failure detected!"
    else
        ok "[5/6] No malloc failures"
        ((pass++))
    fi

    # 6. Ethernet init
    if grep -q "ETH: init ok" "$logfile"; then
        ok "[6/6] Ethernet initialized"
        ((pass++))
    elif grep -q "ETH: init FAILED" "$logfile"; then
        fail "[6/6] Ethernet init failed (check cable)"
    else
        warn "[6/6] No Ethernet init message found"
    fi

    echo ""
    echo "=========================================="
    if [ "$pass" -eq "$total" ]; then
        ok "All $total checks passed!"
    else
        warn "$pass/$total checks passed"
    fi
    info "Full log: $logfile"
}

cmd_sniff() {
    local iface
    iface="${1:-$(find_eth_iface || true)}"
    if [ -z "$iface" ]; then
        fail "No Ethernet interface detected. Specify: $0 sniff <iface>"
        exit 1
    fi

    info "Starting frame sniffer on $iface"
    sudo python3 "$SCRIPT_DIR/frame_sniffer.py" "$iface" -v
}

cmd_heartbeat() {
    local iface
    iface="${1:-$(find_eth_iface || true)}"
    if [ -z "$iface" ]; then
        fail "No Ethernet interface detected. Specify: $0 heartbeat <iface>"
        exit 1
    fi

    info "Sending heartbeats on $iface (node=101, box=1, 10ms interval)"
    info "STM32 should show heartbeat activity (blue LED toggles)"
    sudo python3 "$SCRIPT_DIR/heartbeat_sender.py" "$iface" \
        --node-id 101 --box-id 1 --interval 10
}

cmd_full() {
    echo "============================================"
    echo " T10: Single-Node Raft Hardware Verification"
    echo "============================================"
    echo ""

    # Step 1: Build
    cmd_build
    echo ""

    # Step 2: Flash
    echo "------------------------------------------"
    cmd_flash
    echo ""

    # Step 3: UART monitoring (60s)
    echo "------------------------------------------"
    info "Starting 60s UART capture for verification..."
    cmd_uart_timed 60
    echo ""

    # Step 4: Instructions for manual Ethernet tests
    echo "------------------------------------------"
    info "Automated checks complete. For Ethernet verification:"
    echo ""
    echo "  Terminal 1 (sniffer):    sudo python3 tools/frame_sniffer.py <iface>"
    echo "  Terminal 2 (heartbeat):  sudo python3 tools/heartbeat_sender.py <iface>"
    echo "  Terminal 3 (UART):      ./tools/verify_t10.sh uart"
    echo ""
    info "Watch for:"
    echo "  - Blue LED toggling on heartbeat receipt"
    echo "  - UART: 'health: N hb, 1 nodes tracked'"
    echo "  - Sniffer: HEARTBEAT frames from laptop, HEALTH frames from STM32"
}

# Dispatch
case "${1:-full}" in
    build)     cmd_build ;;
    flash)     cmd_flash ;;
    uart)      cmd_uart ;;
    check)     cmd_uart_timed "${2:-60}" ;;
    sniff)     cmd_sniff "${2:-}" ;;
    heartbeat) cmd_heartbeat "${2:-}" ;;
    full)      cmd_full ;;
    *)
        echo "Usage: $0 {full|build|flash|uart|check [secs]|sniff [iface]|heartbeat [iface]}"
        echo ""
        echo "  full       - Build, flash, verify (default)"
        echo "  build      - Build firmware only"
        echo "  flash      - Flash via ST-LINK"
        echo "  uart       - Monitor UART (interactive)"
        echo "  check [s]  - Capture UART for s seconds, analyze (default: 60)"
        echo "  sniff [if] - Sniff oracle frames on interface"
        echo "  heartbeat [if] - Send test heartbeats"
        exit 1
        ;;
esac
