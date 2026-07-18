# Recovery — a clean way back, before a single custom byte goes on the device

> A bricked HA Voice PE is the one thing you can't easily undo. Back up stock
> firmware and rehearse the recovery path *before* you flash anything custom.

## 1. Back up the stock firmware first

Before your first custom flash, take a full flash dump of the device in its
factory state — **outside** any git repo (it's large and binary):

```bash
# Find the device's port first (see "Finding the device" below).
esptool --port /dev/cu.usbmodemXXXX read-flash 0x0 0x1000000 ~/voice-pe-stock-16MB.bin
shasum -a 256 ~/voice-pe-stock-16MB.bin   # note this hash down somewhere safe
stat -f "%z bytes" ~/voice-pe-stock-16MB.bin   # expect 16777216 (16 MB) for the 16 MB flash variant
```

Keep the file (and its sha256) somewhere durable — this is your guaranteed
way back to exactly what shipped on your device.

**Second line of defense:** also grab the current official
`home-assistant-voice.factory.bin` (+ `.sha256`) for your installed HA Voice
PE firmware version from
[`github.com/esphome/home-assistant-voice-pe/releases`](https://github.com/esphome/home-assistant-voice-pe/releases).
It restores the correct stock layout at offset 0 if your own dump is ever in
doubt.

**Gate check (read-only, safe anytime):** verify the dump's hash and size
match what you noted down before you rely on it.

## 2. Rehearse the recovery path once

```bash
# Bootloader mode: unplug power -> hold the center button (GPIO0) -> plug in
# USB-C -> hold briefly -> release.
# Restore (ONLY if a flash goes wrong):
esptool --port /dev/cu.usbmodemXXXX write-flash 0x0 ~/voice-pe-stock-16MB.bin
```

- Use a **USB data cable** (not charge-only). If the device isn't detected,
  check the board's internal USB-select switch.
- **NEVER** enable Secure Boot / Flash Encryption / `DIS_DOWNLOAD_MODE` — that
  is the one real hard-brick path on this board.
- Do this rehearsal for real (reach bootloader mode, confirm the port shows
  up) **before** a single custom byte goes on the device.

## Finding the device

```bash
esptool.py flash_id   # or: ls /dev/cu.usbmodem*  (macOS)
```

Note the device's serial port and, if you like, its MAC address (`esptool.py
read_mac`) for your own records — useful when you have more than one ESP32
board on your desk.

## Iron rules

- **Never** flip Secure Boot / Flash Encryption / `DIS_DOWNLOAD_MODE`.
- Always use a USB **data** cable, not charge-only.
- Rehearse the bootloader-mode recovery path once before relying on it.
- This backup is created **read-only** (`read-flash`) — it never touches the
  device's contents.
