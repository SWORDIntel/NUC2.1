# Kernel Integration Guide
## Movidius Myriad X VPU Driver

This guide explains how to integrate the Movidius Myriad X VPU driver directly into the Linux kernel source tree instead of building it as an out-of-tree module.

## Overview

Integrating the driver into the kernel provides several advantages:
- Compiled with the same flags and optimizations as the kernel
- Automatic version tracking with kernel releases
- No need to rebuild after kernel updates
- Better integration with kernel subsystems
- Inclusion in distribution kernels

## Prerequisites

- Linux kernel source tree (>= 5.12)
- Build dependencies: `gcc`, `make`, `flex`, `bison`, `libelf-dev`, `libssl-dev`
- Root access for installation

## Integration Steps

### 1. Obtain Kernel Source

```bash
# Download kernel source (example for 6.1.x)
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.tar.xz
tar -xf linux-6.1.tar.xz
cd linux-6.1

# Or use your distribution's kernel source
# Ubuntu/Debian:
apt-get source linux-image-$(uname -r)

# Fedora/RHEL:
dnf download --source kernel
```

### 2. Create Driver Directory

```bash
# Navigate to USB drivers directory
cd drivers/usb/misc

# Create movidius directory
mkdir movidius
cd movidius
```

### 3. Copy Driver Files

Copy the following files into `drivers/usb/misc/movidius/`:

```bash
# Assuming you're in drivers/usb/misc/movidius/
cp /path/to/NUC2.1/movidius_x_vpu.c .
cp /path/to/NUC2.1/vfio_movidius.c .
```

### 4. Create Kconfig

Create `drivers/usb/misc/movidius/Kconfig`:

```kconfig
# SPDX-License-Identifier: GPL-2.0
#
# Movidius Myriad X VPU driver configuration
#

config USB_MOVIDIUS_X_VPU
	tristate "Movidius Myriad X VPU driver"
	depends on USB && IO_URING
	help
	  This driver supports the Intel Movidius Myriad X Vision Processing
	  Unit (VPU), commonly known as the Neural Compute Stick 2 (NCS2).

	  The driver provides a high-performance interface using io_uring for
	  asynchronous inference request submission, zero-copy DMA buffers,
	  adaptive batching, and comprehensive performance monitoring.

	  Features include:
	  - Zero-copy data path with pin_user_pages
	  - io_uring asynchronous interface
	  - Batch submission and adaptive batching
	  - Persistent URB pool for efficient transfers
	  - Multi-device support
	  - CPU affinity and NUMA awareness
	  - Sysfs telemetry for performance monitoring

	  To compile this driver as a module, choose M here: the module
	  will be called movidius_x_vpu.

	  If unsure, say N.

config VFIO_MOVIDIUS
	tristate "VFIO support for Movidius Myriad X VPU"
	depends on USB_MOVIDIUS_X_VPU && VFIO && VFIO_PLATFORM
	help
	  VFIO driver for Movidius Myriad X VPU, enabling device passthrough
	  to virtual machines or direct userspace control via the VFIO API.

	  This driver provides:
	  - Three memory regions (registers, device memory, shared memory)
	  - IRQ support (INTx, MSI, MSI-X with 8 vectors)
	  - Eventfd integration for efficient interrupt handling
	  - Device reset capability
	  - Full VFIO ioctl support

	  Requires IOMMU support for device isolation.

	  To compile this driver as a module, choose M here: the module
	  will be called vfio_movidius.

	  If unsure, say N.
```

### 5. Create Makefile

Create `drivers/usb/misc/movidius/Makefile`:

```makefile
# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Movidius Myriad X VPU drivers
#

obj-$(CONFIG_USB_MOVIDIUS_X_VPU)	+= movidius_x_vpu.o
obj-$(CONFIG_VFIO_MOVIDIUS)		+= vfio_movidius.o
```

### 6. Integrate with Parent Kconfig

Edit `drivers/usb/misc/Kconfig` and add before the final `endmenu`:

```kconfig
source "drivers/usb/misc/movidius/Kconfig"
```

### 7. Integrate with Parent Makefile

Edit `drivers/usb/misc/Makefile` and add:

```makefile
obj-$(CONFIG_USB_MOVIDIUS_X_VPU)	+= movidius/
```

### 8. Configure the Kernel

```bash
# Navigate to kernel root
cd /path/to/linux-6.1

# Configure kernel (choose one method)

# Method 1: Using menuconfig (ncurses interface)
make menuconfig

# Navigate to:
# Device Drivers → USB support → USB Miscellaneous drivers
#   → <M> Movidius Myriad X VPU driver
#   → <M> VFIO support for Movidius Myriad X VPU

# Method 2: Using config file
cat >> .config << EOF
CONFIG_USB_MOVIDIUS_X_VPU=m
CONFIG_VFIO_MOVIDIUS=m
EOF

# Update config with dependencies
make olddefconfig

# Method 3: Built-in (not module)
cat >> .config << EOF
CONFIG_USB_MOVIDIUS_X_VPU=y
CONFIG_VFIO_MOVIDIUS=y
EOF

make olddefconfig
```

### 9. Build the Kernel

```bash
# Build with all CPU cores
make -j$(nproc)

# Build only the modules (faster for testing)
make -j$(nproc) modules

# Build only the Movidius driver
make -j$(nproc) M=drivers/usb/misc/movidius
```

### 10. Install Modules

```bash
# Install all modules
sudo make modules_install

# Or install only Movidius modules
sudo make M=drivers/usb/misc/movidius modules_install

# Update module dependencies
sudo depmod -a
```

### 11. Install Kernel (Optional)

If you built the entire kernel:

```bash
# Install kernel
sudo make install

# Update bootloader (usually automatic)
# For GRUB:
sudo update-grub

# Reboot into new kernel
sudo reboot
```

### 12. Load Modules

```bash
# Load core driver
sudo modprobe movidius_x_vpu

# Load VFIO driver (optional)
sudo modprobe vfio_movidius

# Verify loaded
lsmod | grep movidius

# Check kernel logs
dmesg | grep movidius
```

## Configuration Options

### Module Parameters

Module parameters can be set in several ways:

#### Method 1: Command Line

```bash
sudo modprobe movidius_x_vpu vid=0x03e7 pid=0x2485 batch_delay_ms=5
```

#### Method 2: modprobe.d Configuration

Create `/etc/modprobe.d/movidius.conf`:

```conf
# Movidius X VPU driver configuration
options movidius_x_vpu vid=0x03e7 pid=0x2485
options movidius_x_vpu batch_delay_ms=10
options movidius_x_vpu batch_high_watermark=32
options movidius_x_vpu submission_cpu_affinity=4
```

#### Method 3: Kernel Command Line

Add to kernel boot parameters (e.g., in GRUB):

```
movidius_x_vpu.vid=0x03e7 movidius_x_vpu.batch_delay_ms=5
```

### Available Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `vid` | ushort | 0x03e7 | USB Vendor ID |
| `pid` | ushort | 0x2485 | USB Product ID |
| `batch_delay_ms` | uint | 10 | Adaptive batch delay (ms) |
| `batch_high_watermark` | uint | 32 | Queue depth for immediate dispatch |
| `submission_cpu_affinity` | int | -1 | CPU core for submission thread |

## Automatic Loading

### udev Rules

Create `/etc/udev/rules.d/99-movidius.rules`:

```udev
# Automatically load Movidius driver for NCS2
SUBSYSTEM=="usb", ATTR{idVendor}=="03e7", ATTR{idProduct}=="2485", RUN+="/sbin/modprobe movidius_x_vpu"

# Set permissions for device node
KERNEL=="movidius_x_vpu[0-9]*", MODE="0666", GROUP="users"
```

Reload udev rules:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### systemd Module Loading

Create `/etc/modules-load.d/movidius.conf`:

```
# Load Movidius drivers at boot
movidius_x_vpu
vfio_movidius
```

## Building for Different Architectures

### Cross-Compilation

```bash
# Set cross-compiler
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

# Configure for target
make defconfig

# Enable driver
./scripts/config -m USB_MOVIDIUS_X_VPU
./scripts/config -m VFIO_MOVIDIUS

# Build
make -j$(nproc) M=drivers/usb/misc/movidius
```

### Common Architectures

| Architecture | ARCH | CROSS_COMPILE |
|--------------|------|---------------|
| x86_64 | x86_64 | (native) |
| ARM 32-bit | arm | arm-linux-gnueabihf- |
| ARM 64-bit | arm64 | aarch64-linux-gnu- |
| RISC-V | riscv | riscv64-linux-gnu- |

## Distribution-Specific Integration

### Debian/Ubuntu

```bash
# Install build dependencies
sudo apt-get install build-essential linux-headers-$(uname -r) \
                     libncurses-dev flex bison libssl-dev libelf-dev

# Get kernel source
apt-get source linux-image-$(uname -r)
cd linux-*/

# Follow integration steps above

# Build package
make -j$(nproc) bindeb-pkg

# Install generated .deb packages
sudo dpkg -i ../linux-image-*.deb ../linux-headers-*.deb
```

### Fedora/RHEL/CentOS

```bash
# Install build dependencies
sudo dnf install kernel-devel kernel-headers gcc make ncurses-devel \
                 flex bison openssl-devel elfutils-libelf-devel

# Get kernel source
dnf download --source kernel
rpm -ivh kernel-*.src.rpm
cd ~/rpmbuild/SOURCES

# Follow integration steps above

# Build RPM
make -j$(nproc) binrpm-pkg

# Install generated RPMs
sudo rpm -ivh ~/rpmbuild/RPMS/x86_64/kernel-*.rpm
```

### Arch Linux

```bash
# Install build dependencies
sudo pacman -S base-devel linux-headers

# Clone kernel build scripts
git clone https://aur.archlinux.org/linux-custom.git
cd linux-custom

# Follow integration steps above

# Build package
makepkg -s

# Install
sudo pacman -U linux-custom-*.pkg.tar.zst
```

## Troubleshooting

### Build Errors

#### Missing io_uring support

```
Error: io_uring_cmd not found
```

**Solution**: Ensure kernel version >= 5.12 or disable io_uring requirements:

```bash
# Check kernel version
make kernelversion

# Update to newer kernel if needed
```

#### Missing VFIO symbols

```
Error: vfio_init_group_dev undefined
```

**Solution**: Enable VFIO in kernel config:

```bash
./scripts/config -e VFIO
./scripts/config -e VFIO_PLATFORM
make olddefconfig
```

### Runtime Issues

#### Module not loading

```bash
# Check dependencies
modinfo movidius_x_vpu

# Try manual load with verbose output
sudo modprobe -v movidius_x_vpu

# Check for conflicts
lsmod | grep -i movidius
```

#### Device not detected

```bash
# Verify USB device
lsusb | grep -i movidius

# Check dmesg for errors
dmesg | tail -50 | grep -i movidius

# Try manual USB binding
echo "03e7 2485" | sudo tee /sys/bus/usb/drivers/movidius_x_vpu/new_id
```

## Maintenance

### Updating the Driver

When updating the driver code:

```bash
# Navigate to driver directory
cd drivers/usb/misc/movidius

# Update source files
cp /path/to/new/movidius_x_vpu.c .

# Clean previous build
make M=drivers/usb/misc/movidius clean

# Rebuild
make -j$(nproc) M=drivers/usb/misc/movidius

# Reinstall
sudo make M=drivers/usb/misc/movidius modules_install
sudo depmod -a

# Reload module
sudo rmmod movidius_x_vpu
sudo modprobe movidius_x_vpu
```

### Kernel Upgrades

For minor kernel updates (e.g., 6.1.10 → 6.1.20):

```bash
# Rebuild modules for new kernel
make -j$(nproc) modules
sudo make modules_install
```

For major kernel updates (e.g., 6.1 → 6.6):

1. Review driver compatibility
2. Update driver code if needed
3. Rebuild and reinstall

## Verification

After integration, verify the driver is working:

```bash
# Check module is loaded
lsmod | grep movidius

# Check device nodes exist
ls -l /dev/movidius*

# Check sysfs entries
ls -l /sys/class/movidius_x_vpu/

# Run test application
cd /path/to/NUC2.1
make test
sudo ./test_app

# Monitor kernel logs
dmesg -w | grep movidius
```

## Upstream Submission

If submitting the driver upstream to mainline Linux:

### 1. Prepare Patches

```bash
# Create git commits with proper format
git format-patch -M origin/master

# Check patches
./scripts/checkpatch.pl 0001-*.patch
```

### 2. Test Requirements

- Build test on multiple architectures
- Run sparse (static analysis): `make C=1 M=drivers/usb/misc/movidius`
- Run smatch: `smatch drivers/usb/misc/movidius/movidius_x_vpu.c`
- Test with different kernel configs
- Document all module parameters

### 3. Submission

- Follow Linux kernel coding style
- Include SPDX license identifiers
- Write detailed commit messages
- Send to relevant mailing lists:
  - linux-usb@vger.kernel.org
  - linux-kernel@vger.kernel.org
- CC: relevant maintainers (see `MAINTAINERS` file)

## Additional Resources

- [Linux Kernel Documentation](https://www.kernel.org/doc/html/latest/)
- [Driver API Guide](https://www.kernel.org/doc/html/latest/driver-api/index.html)
- [USB Driver Guidelines](https://www.kernel.org/doc/html/latest/driver-api/usb/index.html)
- [VFIO Documentation](https://www.kernel.org/doc/html/latest/driver-api/vfio.html)
- [Kernel Build System](https://www.kernel.org/doc/html/latest/kbuild/index.html)

---

**Version:** 1.0
**Last Updated:** 2025-11-05
**Kernel Version:** >= 5.12
**Status:** Production Ready
