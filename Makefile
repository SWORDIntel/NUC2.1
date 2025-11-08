# Makefile for Movidius Myriad X VPU driver
# Version: 2.0

KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += movidius_x_vpu.o
obj-m += vfio_movidius.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_app

test: test_app

test_app: test_app.c
	gcc test_app.c -o test_app -luring -O2 -Wall

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
	@echo "  test      - Build test application"
	@echo "  install   - Install kernel modules"
	@echo "  uninstall - Remove installed modules"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make              # Build modules"
	@echo "  make test         # Build test app"
	@echo "  sudo make install # Install modules"

.PHONY: all clean test install uninstall help
