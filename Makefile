# Makefile for Movidius Myriad X VPU driver
# Version: 2.0

KDIR ?= /lib/modules/$(shell uname -r)/build

# Enable io_uring for async performance (set to 0 to disable)
ENABLE_IO_URING ?= 1

# Add compiler flags
ccflags-y := -DMOVIDIUS_ENABLE_IO_URING=$(ENABLE_IO_URING)

obj-m += movidius_x_vpu.o
obj-m += vfio_movidius.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f movidius-bench

bench: movidius-bench

movidius-bench: movidius-bench.c
	gcc movidius-bench.c -o movidius-bench -luring -O2 -Wall -Wextra

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/movidius_x_vpu.ko
	rm -f /lib/modules/$(shell uname -r)/extra/vfio_movidius.ko
	depmod -a

help:
	@echo "Movidius Myriad X VPU Driver Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build kernel modules (default)"
	@echo "  clean     - Clean build artifacts"
	@echo "  bench     - Build benchmark application"
	@echo "  install   - Install kernel modules"
	@echo "  uninstall - Remove installed modules"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  ENABLE_IO_URING=1  - Enable io_uring support (default, requires kernel >= 6.2)"
	@echo "  ENABLE_IO_URING=0  - Disable io_uring, use ioctl only (legacy kernels)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build modules with io_uring"
	@echo "  make ENABLE_IO_URING=0        # Build without io_uring"
	@echo "  make bench                    # Build benchmark tool"
	@echo "  sudo make install             # Install modules"

.PHONY: all clean bench install uninstall help
