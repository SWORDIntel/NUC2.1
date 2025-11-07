# Movidius Device Test Scripts

These scripts help you test the Rust NCAPI implementation with physical Movidius Neural Compute Sticks.

## Prerequisites

1. **Hardware**: One or two Intel Movidius Neural Compute Sticks plugged into USB ports
2. **Kernel Module**: The `movidius_x_vpu` kernel module must be loaded
3. **Permissions**: Read/write access to `/dev/movidius_x_vpu_*` device files

## Quick Start

### 1. Check Device Status

First, verify your devices are detected:

```bash
cd /path/to/NUC2.1/movidius-rs
./scripts/check_devices.sh
```

This will show:
- Kernel module status
- Detected device files
- USB device information
- Recent kernel messages

### 2. Test Single Device

If you have one Movidius stick:

```bash
./scripts/test_single_device.sh
```

This will:
- Verify device is accessible
- Build the Rust NCAPI library in release mode
- Run device enumeration
- Run basic inference (if `test_graph.blob` exists)

### 3. Test Dual Devices

If you have two Movidius sticks:

```bash
./scripts/test_dual_device.sh
```

This will:
- Verify both devices are accessible
- Build the library
- Test parallel inference on both devices
- Show ~2x throughput improvement

## Troubleshooting

### "No Movidius device found"

1. Check USB connection:
   ```bash
   lsusb | grep -i movidius
   ```

2. Load kernel module:
   ```bash
   sudo modprobe movidius_x_vpu
   ```

3. Check device files:
   ```bash
   ls -l /dev/movidius_x_vpu_*
   ```

### "Permission denied"

Add your user to the appropriate group or use sudo:

```bash
sudo chmod 666 /dev/movidius_x_vpu_*
# Or add udev rule for persistent permissions
```

### "Graph blob not found"

To test inference, you need a compiled graph file:

1. Use Intel OpenVINO to compile your model:
   ```bash
   mo.py --input_model model.onnx --output_dir .
   ```

2. Place the resulting `.blob` file as `test_graph.blob`

## Performance Testing

The dual device script demonstrates parallel execution:

- **Single device**: ~30 FPS (baseline)
- **Dual devices**: ~60 FPS (2x throughput)

Actual performance depends on:
- Model complexity
- Input resolution
- USB bandwidth
- Host CPU overhead

## Script Details

### check_devices.sh
- No arguments required
- Safe to run anytime
- Does not modify system state

### test_single_device.sh
- Requires 1 device
- Builds in release mode
- Runs enumeration and basic tests

### test_dual_device.sh
- Requires 2 devices
- Tests parallel execution
- Shows performance comparison

## Examples Used

The scripts run these Rust examples:

- `device_enumerate` - Lists all detected devices
- `basic_inference` - Single device inference
- `parallel_inference` - Dual device parallel inference

Run examples manually:
```bash
cargo run --release --example device_enumerate
cargo run --release --example basic_inference
cargo run --release --example parallel_inference
```

## Development Workflow

1. **Check devices**: `./scripts/check_devices.sh`
2. **Make code changes**: Edit Rust source files
3. **Test**: Run appropriate test script
4. **Iterate**: Repeat as needed

## Notes

- Scripts use `set -e` (exit on error) for safety
- Release builds are used for maximum performance
- Debug builds add ~10-20% overhead
- All scripts are safe and non-destructive

## Support

For issues:
1. Run `./scripts/check_devices.sh` and save output
2. Check `dmesg | grep movidius` for kernel messages
3. Verify USB connection with `lsusb`
4. Check driver version with `modinfo movidius_x_vpu`
