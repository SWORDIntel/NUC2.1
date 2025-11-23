# Makefile for Movidius Myriad X VPU driver
# Version: 2.2 - Production Hardening

KDIR ?= /lib/modules/$(shell uname -r)/build

# Enable io_uring for async performance (set to 0 to disable)
ENABLE_IO_URING ?= 1

# Detect liburing availability
HAS_LIBURING := $(shell pkg-config --exists liburing 2>/dev/null && echo 1 || \
                 (test -f /usr/include/liburing.h && echo 1 || \
                 (test -f /usr/local/include/liburing.h && echo 1 || echo 0)))

# Add compiler flags
ccflags-y := -DMOVIDIUS_ENABLE_IO_URING=$(ENABLE_IO_URING)

obj-m += movidius_x_vpu.o
obj-m += vfio_movidius.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

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
	@echo "Building movidius-bench with io_uring support..."
	gcc movidius-bench.c -o movidius-bench -luring -lm -O2 -Wall -Wextra -DHAS_LIBURING=1
else
	@echo "Building movidius-bench in ioctl-only mode (liburing not available)..."
	gcc movidius-bench.c -o movidius-bench -lm -O2 -Wall -Wextra -DHAS_LIBURING=0
endif
else
	@echo "Building movidius-bench in ioctl-only mode (io_uring disabled)..."
	gcc movidius-bench.c -o movidius-bench -lm -O2 -Wall -Wextra -DHAS_LIBURING=0
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
	@echo "Movidius Myriad X VPU Driver Build System v2.2"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build kernel modules (default)"
	@echo "  clean     - Clean build artifacts"
	@echo "  bench     - Build benchmark application (auto-detects liburing)"
	@echo "  test      - Run integration tests"
	@echo "  install   - Install kernel modules"
	@echo "  uninstall - Remove installed modules"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  ENABLE_IO_URING=1  - Enable io_uring support (default, requires kernel >= 6.2)"
	@echo "  ENABLE_IO_URING=0  - Disable io_uring, use ioctl only (legacy kernels)"
	@echo ""
	@echo "System Detection:"
	@echo "  liburing detected: $(HAS_LIBURING)"
	@echo "  io_uring enabled:  $(ENABLE_IO_URING)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build modules with io_uring"
	@echo "  make ENABLE_IO_URING=0        # Build without io_uring"
	@echo "  make bench                    # Build benchmark tool (auto-detects deps)"
	@echo "  make test                     # Run full test suite"
	@echo "  sudo make install             # Install modules"

.PHONY: all clean bench test check-bench-deps install uninstall help
