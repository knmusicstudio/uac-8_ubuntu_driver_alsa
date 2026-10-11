Markdown
# snd-uac8 (Experimental)

Experimental in-tree / out-of-tree ALSA kernel driver for the **ZOOM UAC-8** USB 3.0 audio interface (USB ID `1686:f02b`).

> ⚠️ **DISCLAIMER: EXPERIMENTAL SOFTWARE**  
> This kernel driver is an experimental project in active development. While designed with safety checks (preventing kernel panics, memory leaks, and deadlock avoidance), loading custom out-of-tree kernel modules carries inherent risks. Use at your own risk.

---

## Overview

The ZOOM UAC-8 uses non-standard USB bulk streaming endpoints instead of standard isochronous UAC2 streaming. Previously, running this device under Linux required a custom userspace bridge (libusb + JACK).

This project implements a native ALSA PCM driver (`snd-uac8`) running directly in kernel space:
- **Direct ALSA Integration**: Native ALSA PCM nodes (`hw:X,0`), accessible by JACK, PipeWire, and ALSA-aware DAWs.
- **Bulk URB Ring Pipeline**: Pre-allocated 128-URB queue handling continuous bulk transfers without userspace ringbuffer jitter.
- **20-channel Playback / 18-channel Capture**: Native multi-channel streaming at 96 kHz (32-bit LE).
- **Concurrency & Panic Safeguards**: Spinlocks guarding ISR callbacks, atomic error counters, and delayed work for graceful XRUN recovery.

---

## Requirements

- Linux Kernel 6.x / 7.x
- Kernel build headers (`linux-headers-$(uname -r)`)
- `build-essential`

```bash
sudo apt install build-essential linux-headers-$(uname -r)
