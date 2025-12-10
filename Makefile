# Makefile for Movidius Myriad X VPU driver
# Version: 2.3 - Meteor Lake Optimized

KDIR ?= /lib/modules/$(shell uname -r)/build

# Enable io_uring for async performance (set to 0 to disable)
ENABLE_IO_URING ?= 1

# Detect liburing availability
HAS_LIBURING := $(shell pkg-config --exists liburing 2>/dev/null && echo 1 || \
                 (test -f /usr/include/liburing.h && echo 1 || \
                 (test -f /usr/local/include/liburing.h && echo 1 || echo 0)))

# Meteor Lake optimization flags
# Source meteor-lake-flags.sh before running make, or use defaults below
# Example: source meteor-lake-flags.sh && make bench

# User-space C flags (override via CFLAGS_USERSPACE env var)
CFLAGS_USERSPACE ?= -O3 -march=meteorlake -mtune=meteorlake -mavx2 -mfma -mavxvnni -mavxvnniint8 -mavxifma -mavxneconvert -pipe -flto=auto -funroll-loops -ftree-vectorize

# User-space linker flags (override via LDFLAGS_USERSPACE env var)
LDFLAGS_USERSPACE ?= -flto=auto -Wl,--gc-sections -Wl,--as-needed

# Kernel module flags (override via KCFLAGS env var)
# Note: Kernel modules have restrictions - these are safe flags
KCFLAGS_OPTIMAL ?= -O3 -march=meteorlake -mtune=meteorlake -mavx2 -mfma -mavxvnni -mavxvnniint8 -falign-functions=64 -falign-loops=64

# Add compiler flags
ccflags-y := -DMOVIDIUS_ENABLE_IO_URING=$(ENABLE_IO_URING)

# Kernel module flags (can be overridden via KCFLAGS env var)
KCFLAGS ?= $(KCFLAGS_OPTIMAL)

obj-m += movidius_x_vpu.o
obj-m += vfio_movidius.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) KCFLAGS="$(KCFLAGS)" modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f movidius-bench movidius-bench-ioctl

bench: check-bench-deps movidius-bench

check-bench-deps:
ifeq ($(ENABLE_IO_URING),1)
ifeq ($(HAS_LIBURING),0)
	@echo "========================================="
	@echo "WARNING: liburing not found!"
	@echo "========================================="
	@echo ""
	@echo "Building ioctl-only benchmark tool instead."
	@echo ""
	@echo "To install liburing:"
	@echo "  Ubuntu/Debian: sudo apt-get install liburing-dev"
	@echo "  RHEL/Fedora:   sudo dnf install liburing-devel"
	@echo ""
	@echo "To build with io_uring support, install liburing and run 'make bench' again."
	@echo "========================================="
	@echo ""
endif
endif

movidius-bench: movidius-bench.c
ifeq ($(ENABLE_IO_URING),1)
ifeq ($(HAS_LIBURING),1)
	@echo "Building movidius-bench with io_uring support (Meteor Lake optimized)..."
	gcc $(CFLAGS_USERSPACE) -Wall -Wextra movidius-bench.c -o movidius-bench -luring -lm $(LDFLAGS_USERSPACE) -DHAS_LIBURING=1
else
	@echo "Building movidius-bench in ioctl-only mode (liburing not available, Meteor Lake optimized)..."
	gcc $(CFLAGS_USERSPACE) -Wall -Wextra movidius-bench.c -o movidius-bench -lm $(LDFLAGS_USERSPACE) -DHAS_LIBURING=0
endif
else
	@echo "Building movidius-bench in ioctl-only mode (io_uring disabled, Meteor Lake optimized)..."
	gcc $(CFLAGS_USERSPACE) -Wall -Wextra movidius-bench.c -o movidius-bench -lm $(LDFLAGS_USERSPACE) -DHAS_LIBURING=0
endif

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/movidius_x_vpu.ko
	rm -f /lib/modules/$(shell uname -r)/extra/vfio_movidius.ko
	depmod -a

test: bench
	@echo "========================================="
	@echo "Running integration tests..."
	@echo "========================================="
	@if [ -f movidius-bench ]; then \
		./scripts/run-tests.sh; \
	else \
		echo "Error: movidius-bench not built. Run 'make bench' first."; \
		exit 1; \
	fi

help:
	@echo "Movidius Myriad X VPU Driver Build System v2.3 - Meteor Lake Optimized"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build kernel modules (default, uses Meteor Lake optimizations)"
	@echo "  clean     - Clean build artifacts"
	@echo "  bench     - Build benchmark application (auto-detects liburing, Meteor Lake optimized)"
	@echo "  test      - Run integration tests"
	@echo "  install   - Install kernel modules"
	@echo "  uninstall - Remove installed modules"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  ENABLE_IO_URING=1  - Enable io_uring support (default, requires kernel >= 6.2)"
	@echo "  ENABLE_IO_URING=0  - Disable io_uring, use ioctl only (legacy kernels)"
	@echo "  KCFLAGS=...        - Override kernel module compiler flags"
	@echo ""
	@echo "Optimization:"
	@echo "  Source meteor-lake-flags.sh before building for optimal Meteor Lake performance"
	@echo "  Example: source meteor-lake-flags.sh && make bench"
	@echo ""
	@echo "System Detection:"
	@echo "  liburing detected: $(HAS_LIBURING)"
	@echo "  io_uring enabled:  $(ENABLE_IO_URING)"
	@echo ""
	@echo "Examples:"
	@echo "  source meteor-lake-flags.sh && make      # Build with Meteor Lake optimizations"
	@echo "  make ENABLE_IO_URING=0                    # Build without io_uring"
	@echo "  make bench                                # Build benchmark tool (auto-detects deps)"
	@echo "  make test                                 # Run full test suite"
	@echo "  sudo make install                         # Install modules"

.PHONY: all clean bench test check-bench-deps install uninstall help
