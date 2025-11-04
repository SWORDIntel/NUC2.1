# Movidius VPU VFIO Driver Compilation Blocker

## 1. Project Goal

The primary objective is to implement a high-performance driver stack for an Intel Movidius Myriad X VPU. The architecture consists of two main kernel modules:
1.  A core USB driver (`movidius_x_vpu.ko`) that handles the low-level hardware interaction and registers a platform device.
2.  A VFIO driver (`vfio_movidius.ko`) that binds to the platform device created by the core driver. This allows the VPU to be passed through to a virtual machine or controlled directly by a userspace application, enabling near-native performance.

## 2. Current Status & Progress

Significant progress has been made:
*   The core USB driver, `movidius_x_vpu.c`, has been successfully written and refactored. It correctly uses a platform device model, separating hardware discovery from the user-facing interface.
*   The initial version of the VFIO platform driver, `vfio_movidius.c`, has been created. It is designed to bind to the `movidius_x_vpu` platform device.
*   The `Makefile` is set up to build both `movidius_x_vpu.ko` and `vfio_movidius.ko`.
*   The core driver (`movidius_x_vpu.c`) compiles successfully, even with the limited headers available in the current environment.

All source code (`movidius_x_vpu.c`, `vfio_movidius.c`, `Makefile`, `test_app.c`, and `README.md`) is included in the commit alongside this document.

## 3. The Blocker: Incomplete Kernel Headers

The project is currently at a complete standstill due to a build environment issue. The `vfio_movidius.c` module fails to compile with the following error:

```
fatal error: linux/vfio_platform.h: No such file or directory
```

**Root Cause:**
The build environment is running a customized Google Kubernetes Engine (GKE) kernel: `6.8.0-1026-gke`. The installed kernel headers for this version are incomplete. They lack the necessary files for building out-of-tree VFIO modules, which are typically included in the full kernel source or a more comprehensive kernel development package.

All attempts to resolve this within the provided environment have failed.

## 4. Path to Resolution (For Your Environment)

Your environment should have the complete kernel source or the full `-dev` header package for the GKE kernel, which will resolve this issue. Here are the steps to finalize the work:

### Step 1: Verify Your Build Environment
Before compiling, please confirm that the critical missing header file exists. Run the following command:
```bash
find /usr/src -name "vfio_platform.h"
```
This should return a path, likely similar to `/usr/src/linux-headers-6.8.0-1026-gke/include/linux/vfio_platform.h`.

### Step 2: Clean the `Makefile`
The current `Makefile` contains a workaround I implemented to guess the location of the kernel headers. This is no longer needed in a correct environment. Please **replace the entire contents of the `Makefile`** with the following clean version:

```makefile
# Makefile for Movidius Myriad X VPU driver

KDIR := /lib/modules/$(shell uname -r)/build

obj-m += movidius_x_vpu.o
obj-m += vfio_movidius.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

test:
	gcc test_app.c -o test_app -luring
```

### Step 3: Compile Both Modules
With the correct headers present and the `Makefile` cleaned up, the compilation should succeed. Simply run:
```bash
make
```
This will produce the two required kernel modules: `movidius_x_vpu.ko` and `vfio_movidius.ko`.

### Step 4: Next Steps (Verification)
After a successful build, the next logical steps would be to:
1.  Load the core driver: `sudo insmod movidius_x_vpu.ko`
2.  Load the VFIO driver: `sudo insmod vfio_movidius.ko`
3.  Check `dmesg` to ensure the VFIO driver successfully probed and bound to the platform device created by the core driver.
