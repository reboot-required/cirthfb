# cirthfb

[Project Overview](https://github.com/reboot-required/cirthfb-yocto/wiki/Project-Overview)

[Project Plan](https://github.com/users/reboot-required/projects/1/views/1)

[Project Schedule](https://github.com/reboot-required/cirthfb-yocto/wiki/Schedule)

A Linux framebuffer driver for the Waveshare 2.13" e-ink display HAT (V4), targeting the Raspberry Pi Zero 2W.

![License: GPL 2.0](https://img.shields.io/badge/License-GPL%202.0-green.svg)

Named after the Cirth, the runic script of Middle-earth, carved in stone and readable without power or light. Like those runes, this framebuffer driver renders information onto e-ink.

---

## Components

- **`cirthfb.ko`** — out-of-tree Linux kernel module, implements the SPI and framebuffer subsystem interface
- **`cirthfbd`** — userspace daemon, reads system metrics and writes rendered frames to `/dev/fb1`

---

## How it works

`cirthfbd` and `cirthfb.ko` share `/dev/fb1` as a 1 bpp shadow buffer in kernel RAM. Neither component pushes pixels to the display on every write — a full SPI transfer is triggered explicitly once per update cycle.

### Update cycle (every 30 s by default)

```
cirthfbd (userspace)
├── render_clear()        writes 0xFF into the mmap'd shadow buffer
├── draw_string() ×4      sets pixel bits in the shadow buffer
│                         (all RAM only — no SPI, no display update yet)
└── render_flush()
        │
        └── pwrite(fb_fd, fb_buf, FB_SIZE, 0)
                │
                │  syscall → VFS → fbmem.c → cirthfb_write()
                │  copy_from_user() → screen_base
                │
                └── cirthfb_flush()  [cirthfb.ko]
                        ├── resets EPD RAM address counter to (0, 0)
                        ├── rotates buffer 90° CW  (landscape FB → portrait display)
                        └── SPI transfer → Waveshare EPD2in13V4
```

### Why `pwrite()` for flush?

`cirthfbd` renders into a `mmap`'d view of the framebuffer. Writes through `mmap` reach the kernel's `screen_base` only after `copy_from_user` — there is no automatic SPI transfer on pixel writes.

`pwrite()` at offset 0 routes through `cirthfb_write` → `cirthfb_flush`, which resets the EPD RAM address counter, rotates the buffer, and sends the full frame over SPI. It also handles ARM cache coherency via `copy_from_user`, ensuring that CPU-cached mmap writes are visible to the kernel before the SPI transfer starts.

`ioctl(FBIO_WAITFORVSYNC)` was the original flush mechanism but delegates to `fb_ioctl` in Linux 6.x rather than `fb_sync`, making it a no-op for drivers that do not implement `fb_ioctl`.

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
make KERNEL_SRC=/path/to/rpi-kernel-build
```

Clean with:

```bash
make KERNEL_SRC=/path/to/rpi-kernel-build clean
```

#### Module parameters

Override GPIO numbers at load time if they differ from the defaults:

```bash
insmod cirthfb.ko dc_gpio=537 rst_gpio=529 busy_gpio=536
```

| Parameter   | Default | Description      |
|-------------|---------|------------------|
| `dc_gpio`   | 537     | DC pin (BCM 25)  |
| `rst_gpio`  | 529     | RST pin (BCM 17) |
| `busy_gpio` | 536     | BUSY pin (BCM 24)|

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

On successful probe the display initialises and clears to white.

### Unbind

```bash
echo spi0.0 > /sys/bus/spi/drivers/cirthfb/unbind
echo > /sys/bus/spi/devices/spi0.0/driver_override
```

---

## Yocto Integration

For a fully automated build and image that loads `cirthfb` at boot via Device Tree overlay, see the [cirthfb-yocto](https://github.com/reboot-required/cirthfb-yocto) repository.

---

## License

GPL 2.0 — see [LICENSE](./LICENSE)
