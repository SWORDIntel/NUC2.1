#!/bin/bash
# Test script for single Movidius Neural Compute Stick
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=================================="
echo "Single Device Test Script"
echo "=================================="
echo ""

# Check if device exists
if [ ! -e "/dev/movidius_x_vpu_0" ]; then
    echo "ERROR: No Movidius device found at /dev/movidius_x_vpu_0"
    echo "Please ensure:"
    echo "  1. Movidius stick is plugged in"
    echo "  2. Kernel module is loaded (modprobe movidius_x_vpu)"
    echo "  3. You have permissions to access the device"
    exit 1
fi

echo "✓ Device found: /dev/movidius_x_vpu_0"
ls -l /dev/movidius_x_vpu_0
echo ""

# Build the library
echo "Building Rust NCAPI library..."
cd "$ROOT_DIR"
cargo build --release
echo "✓ Build successful"
echo ""

# Run basic device test
echo "Running device enumeration test..."
cargo run --release --example device_enumerate
echo ""

# Run basic inference test if graph file exists
if [ -f "test_graph.blob" ]; then
    echo "Running basic inference test..."
    cargo run --release --example basic_inference
else
    echo "⚠ No test_graph.blob found, skipping inference test"
    echo "  To test inference, place a compiled graph blob in $ROOT_DIR"
fi

echo ""
echo "=================================="
echo "Single device test completed!"
echo "=================================="
