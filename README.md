# cirthfb

[Project Overview](https://github.com/reboot-required/cirthfb-yocto/wiki/Project-Overview)

[Project Plan](https://github.com/users/reboot-required/projects/1/views/1)

[Project Schedule](https://github.com/reboot-required/cirthfb-yocto/wiki/Schedule)

A Linux framebuffer driver for the Waveshare 2.13" e-ink display HAT (V4), targeting the Raspberry Pi Zero 2W.

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

Named after the Cirth, the runic script of Middle-earth, carved in stone and readable without power or light. Like those runes, this framebuffer driver renders information onto e-ink.

---

## Components

- **`cirthfb.ko`** — out-of-tree Linux kernel module, implements the SPI and framebuffer subsystem interface
- **`cirthfbd`** — userspace daemon, reads system metrics and writes rendered frames to `/dev/fb0`

---

## Hardware

| Component | Details |
|---|---|
| SBC | Raspberry Pi Zero 2W |
| Display | Waveshare 2.13" e-ink HAT V4 |
| Interface | SPI |

### Wiring

| Signal | BCM | Linux GPIO (Pi Zero 2W / Scarthgap) |
|--------|-----|-------------------------------------|
| DC     | 25  | 537                                 |
| RST    | 17  | 529                                 |
| BUSY   | 24  | 536                                 |
| SCLK   | 11  | SPI0 CLK                            |
| MOSI   | 10  | SPI0 MOSI                           |
| CS     | 8   | SPI0 CE0                            |

---

## Building

### Prerequisites

- `aarch64-poky-linux-` cross-compile toolchain
- Raspberry Pi kernel build tree (or a sourced Yocto SDK environment)
- `make`

### Kernel Module

Build against an external kernel tree or a sourced SDK environment:

```bash
make KERNELDIR=/path/to/rpi-kernel-build
```

Build directly against a local Yocto work tree (no arguments needed if `YOCTO_BUILD` is set correctly in the Makefile):

```bash
make local
```

Clean either way by substituting `clean` or `local-clean` respectively.

#### Module parameters

Override GPIO numbers at load time if they differ from the defaults:

```bash
insmod cirthfb.ko dc_gpio=537 rst_gpio=529 busy_gpio=536
```

| Parameter   | Default              | Description      |
|-------------|----------------------|------------------|
| `dc_gpio`   | 537                  | DC pin (BCM 25)  |
| `rst_gpio`  | 529                  | RST pin (BCM 17) |
| `busy_gpio` | 536                  | BUSY pin (BCM 24)|
| `display`   | `waveshare2in13v4`   | Display type     |

---

## Usage

No Device Tree overlay is required for testing. Bind the driver manually via sysfs.

### 1. Load the module

```bash
insmod cirthfb.ko
```

### 2. Unbind `spidev` if it currently owns `spi0.0`

```bash
echo spi0.0 > /sys/bus/spi/drivers/spidev/unbind
```

### 3. Override driver selection and trigger probe

```bash
echo cirthfb > /sys/bus/spi/devices/spi0.0/driver_override
echo spi0.0 > /sys/bus/spi/drivers/cirthfb/bind
```

On successful probe the display initialises, clears to white, and renders a checkerboard test pattern.

### Unbind

```bash
echo spi0.0 > /sys/bus/spi/drivers/cirthfb/unbind
echo > /sys/bus/spi/devices/spi0.0/driver_override
```

---

## Yocto Integration

For a fully automated build and image that loads `cirthfb` at boot, see the [cirthfb-yocto](https://github.com/reboot-required/cirthfb-yocto) repository.

---

## License

MIT — see [LICENSE](./LICENSE)
