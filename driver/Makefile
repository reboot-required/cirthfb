# Cross-compile for Raspberry Pi Zero 2W (Cortex-A53, arm64)
#
# make local              — build against local Yocto work tree (no args needed)
# make local-clean        — clean local build artifacts
# make KERNELDIR=...      — build with external kernel tree (or sourced SDK env)

ARCH          ?= arm64
CROSS_COMPILE ?= aarch64-poky-linux-
# module.bbclass passes KERNEL_PATH; local builds pass KERNELDIR directly.
KERNELDIR     ?= $(KERNEL_PATH)
ifeq ($(KERNELDIR),)
$(error Set KERNELDIR (or KERNEL_PATH) to the RPi kernel build tree)
endif

obj-m := cirthfb.o

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

# ── local targets: build directly against the Yocto work tree ─────────────────
YOCTO_BUILD ?= .  # Set to the top-level Yocto build directory if not already set by the environment
_SR  = $(YOCTO_BUILD)/tmp/sysroots-components/x86_64
_BIN = $(_SR)/binutils-cross-aarch64/usr/bin/aarch64-poky-linux
_CC  = $(_SR)/gcc-cross-aarch64/usr/bin/aarch64-poky-linux/aarch64-poky-linux-gcc \
       -B$(_BIN)/aarch64-poky-linux-
_KDIR = $(YOCTO_BUILD)/tmp/work/raspberrypi0_2w_64-poky-linux/linux-raspberrypi/6.6.63+git/linux-raspberrypi0_2w_64-standard-build

# gcc-cross libexec/as is a broken symlink to binutils-cross; -B fixes the lookup.
# PATH only needed for the linker (aarch64-poky-linux-ld).
define local_make
	PATH="$(_BIN):$$PATH" \
	$(MAKE) -C $(_KDIR) M=$(CURDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) CC="$(_CC)" $(1)
endef

local:
	$(call local_make,modules)

local-clean:
	$(call local_make,clean)

.PHONY: all clean local local-clean
