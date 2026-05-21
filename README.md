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

TBD

---

## Building

### Prerequisites

- Linux kernel headers for your target kernel
- Cross-compile toolchain for `aarch64`
- `make`

### Kernel Module

```bash

```

### Userspace Daemon

```bash

```

---

## Usage

TBD

---

## Yocto Integration

For a fully automated build and image that loads `cirthfb` at boot, see the [cirthfb-yocto](https://github.com/reboot-required/cirthfb-yocto) repository.

---

## License

MIT — see [LICENSE](./LICENSE)
