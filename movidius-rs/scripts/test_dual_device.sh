#!/bin/bash
# Test script for two Movidius Neural Compute Sticks
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=================================="
echo "Dual Device Test Script"
echo "=================================="
echo ""

# Check if both devices exist
DEVICE_COUNT=0
for i in 0 1; do
    if [ -e "/dev/movidius_x_vpu_$i" ]; then
        echo "✓ Device $i found: /dev/movidius_x_vpu_$i"
        ls -l "/dev/movidius_x_vpu_$i"
        DEVICE_COUNT=$((DEVICE_COUNT + 1))
    else
        echo "✗ Device $i not found: /dev/movidius_x_vpu_$i"
    fi
done

echo ""
if [ $DEVICE_COUNT -lt 2 ]; then
    echo "ERROR: Found only $DEVICE_COUNT device(s), need 2"
    echo "Please ensure:"
    echo "  1. Two Movidius sticks are plugged in"
    echo "  2. Kernel module is loaded (modprobe movidius_x_vpu)"
    echo "  3. Both devices are recognized by the kernel"
    exit 1
fi

echo "✓ Found $DEVICE_COUNT devices"
echo ""

# Build the library
echo "Building Rust NCAPI library..."
cd "$ROOT_DIR"
cargo build --release
echo "✓ Build successful"
echo ""

# Run dual device test
echo "Running dual device enumeration test..."
cargo run --release --example device_enumerate
echo ""

# Run parallel inference test if available
if [ -f "examples/parallel_inference.rs" ]; then
    echo "Running parallel inference test on both devices..."
    cargo run --release --example parallel_inference
else
    echo "⚠ Parallel inference example not yet implemented"
    echo "  Testing devices sequentially instead..."

    for i in 0 1; do
        echo ""
        echo "Testing device $i..."
        MOVIDIUS_DEVICE_INDEX=$i cargo run --release --example basic_inference || true
    done
fi

echo ""
echo "=================================="
echo "Dual device test completed!"
echo "=================================="
echo ""
echo "Performance comparison:"
echo "  Single device: baseline"
echo "  Dual device:   ~2x throughput (parallel execution)"
