#!/bin/bash
# Check status of Movidius devices

echo "=================================="
echo "Movidius Device Status Check"
echo "=================================="
echo ""

# Check kernel module
echo "1. Checking kernel module..."
if lsmod | grep -q movidius; then
    echo "✓ Kernel module loaded:"
    lsmod | grep movidius
else
    echo "✗ Kernel module NOT loaded"
    echo "  Run: sudo modprobe movidius_x_vpu"
fi
echo ""

# Check device files
echo "2. Checking device files..."
FOUND_DEVICES=0
for i in {0..7}; do
    if [ -e "/dev/movidius_x_vpu_$i" ]; then
        echo "✓ Device $i: /dev/movidius_x_vpu_$i"
        ls -lh "/dev/movidius_x_vpu_$i"
        FOUND_DEVICES=$((FOUND_DEVICES + 1))
    fi
done

if [ $FOUND_DEVICES -eq 0 ]; then
    echo "✗ No device files found in /dev/movidius_x_vpu_*"
else
    echo ""
    echo "✓ Found $FOUND_DEVICES device(s)"
fi
echo ""

# Check USB devices
echo "3. Checking USB devices..."
if command -v lsusb &> /dev/null; then
    # Intel Movidius VID:PID patterns
    # VID 03e7 = Intel Movidius
    if lsusb | grep -i "03e7"; then
        echo "✓ Movidius USB device(s) detected:"
        lsusb | grep -i "03e7"
    else
        echo "✗ No Movidius USB devices found"
        echo "  Expected VID: 03e7 (Intel Movidius)"
    fi
else
    echo "⚠ lsusb command not available"
fi
echo ""

# Check dmesg for recent messages
echo "4. Recent kernel messages (last 20 lines)..."
if command -v dmesg &> /dev/null; then
    dmesg | grep -i movidius | tail -20 || echo "  (no movidius messages in dmesg)"
else
    echo "⚠ dmesg command requires sudo privileges"
fi
echo ""

# Summary
echo "=================================="
echo "Summary:"
echo "  Kernel module: $(lsmod | grep -q movidius && echo 'LOADED' || echo 'NOT LOADED')"
echo "  Device count:  $FOUND_DEVICES"
if [ $FOUND_DEVICES -gt 0 ]; then
    echo ""
    echo "✓ Ready for testing!"
    echo "  Single device: ./scripts/test_single_device.sh"
    [ $FOUND_DEVICES -ge 2 ] && echo "  Dual device:   ./scripts/test_dual_device.sh"
else
    echo ""
    echo "✗ Not ready - no devices found"
    echo "  1. Plug in Movidius stick(s)"
    echo "  2. Load kernel module: sudo modprobe movidius_x_vpu"
    echo "  3. Re-run this script"
fi
echo "=================================="
