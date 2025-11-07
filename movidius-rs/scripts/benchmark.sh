#!/bin/bash
# Convenience script to launch the TUI benchmark tool
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=========================================="
echo "Movidius Neural Compute Stick Benchmark"
echo "=========================================="
echo ""

# Check if devices exist
DEVICE_COUNT=0
for i in 0 1; do
    if [ -e "/dev/movidius_x_vpu_$i" ]; then
        echo "✓ Device $i found"
        DEVICE_COUNT=$((DEVICE_COUNT + 1))
    fi
done

if [ $DEVICE_COUNT -eq 0 ]; then
    echo ""
    echo "ERROR: No Movidius devices found"
    echo "Please ensure:"
    echo "  1. Movidius stick(s) are plugged in"
    echo "  2. Kernel module is loaded (modprobe movidius_x_vpu)"
    echo "  3. Device files exist in /dev/movidius_x_vpu_*"
    exit 1
fi

echo ""
echo "Found $DEVICE_COUNT device(s)"
echo "Launching TUI benchmark..."
echo ""

cd "$ROOT_DIR"
cargo run --release -p movidius-bench
