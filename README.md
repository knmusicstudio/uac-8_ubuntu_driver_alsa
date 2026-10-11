# snd-uac8 (Experimental)

Experimental ALSA Linux kernel driver for ZOOM UAC-8 (USB 1686:f02b).

## Prerequisites

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

## Installation

### 1. Build

```bash
make
```

### 2. Install kernel module

```bash
sudo cp snd-uac8.ko /lib/modules/$(uname -r)/kernel/drivers/sound/
sudo depmod -a
```

### 3. Load the module

```bash
sudo modprobe snd-uac8
```

To enable extended sample rates (44.1k / 48k / 88.2k / 176.4k / 192k Hz; UNTESTED):

```bash
sudo modprobe snd-uac8 ext_rates=1
```

### 4. Verify

```bash
sudo dmesg | grep snd-uac8
cat /proc/asound/cards
```

Verify that the output shows `ZOOM UAC-8 ready: playback 20ch / capture 18ch` and that the card appears in `/proc/asound/cards`.

## Uninstallation

```bash
sudo rmmod snd-uac8
sudo rm -f /lib/modules/$(uname -r)/kernel/drivers/sound/snd-uac8.ko
sudo depmod -a
```
