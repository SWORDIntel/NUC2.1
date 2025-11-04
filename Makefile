# Makefile for Movidius Myriad X VPU driver

# Specify the kernel version to build against
KVERSION ?= $(shell uname -r)
KDIR := /lib/modules/$(KVERSION)/build

# If the default KDIR doesn't exist, try to find a suitable alternative
ifeq ($(wildcard $(KDIR)),)
    ALT_KDIR := $(lastword $(sort $(wildcard /usr/src/linux-headers-6.8.0-*-gke/)))
    ifneq ($(ALT_KDIR),)
        KDIR := $(ALT_KDIR)
        $(warning KDIR not found, using alternative: $(KDIR))
    else
        $(error Kernel headers for $(KVERSION) not found. Please install them.)
    endif
endif

obj-m += movidius_x_vpu.o
obj-m += vfio_movidius.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

test:
	gcc test_app.c -o test_app -luring
