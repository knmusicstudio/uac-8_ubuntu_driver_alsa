# snd-uac8 (Experimental)

Experimental ALSA Linux kernel driver for the **ZOOM UAC-8** USB 3.0 audio interface (USB ID `1686:f02b`).

## Prerequisites

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

## Quick Installation (Recommended)

Run the automated script to build, install, register autoloading, handle WirePlumber contention, and load the driver:

```bash
sudo ./install.sh
```

## Manual Installation

### 1. Build
```bash
make
```

### 2. Install Kernel Module
```bash
sudo mkdir -p /lib/modules/$(uname -r)/extra/
sudo cp snd-uac8.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
```

### 3. Load Module
```bash
sudo modprobe snd-uac8
```

To enable extended sample rates (44.1 / 48 / 88.2 / 176.4 / 192 kHz, untested):
```bash
sudo modprobe snd-uac8 ext_rates=1
```

## Verification

Check the kernel ring buffer logs and ALSA card list:
```bash
sudo dmesg | grep snd-uac8
cat /proc/asound/cards
```

Verify that `ZOOM UAC-8 ready: playback 20ch / capture 18ch` is displayed and `snd-uac8` is listed.

## Uninstallation

Run the automated uninstallation script:
```bash
sudo ./uninstall.sh
```

Or manually:
```bash
sudo rmmod snd-uac8
sudo rm -f /lib/modules/$(uname -r)/extra/snd-uac8.ko
sudo rm -f /etc/modules-load.d/snd-uac8.conf
sudo depmod -a
```
