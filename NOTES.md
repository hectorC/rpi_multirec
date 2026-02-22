# rpi_multirec notes

## Summary
- C++ minimal ALSA capture prototype that writes RF64 WAV via libsndfile.
- 48/96 kHz selectable, default 84 channels, 24-bit packed (`S24_3LE`).
- Capture thread writes into a ring buffer; writer thread drains to disk.
- XRUNs and dropped bytes are reported on shutdown.

## Build (Ubuntu Server on Raspberry Pi)
Install dependencies:
```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libasound2-dev libsndfile1-dev
```

Configure and build:
```bash
cmake -S . -B build
cmake --build build -j
```

## Run
Example (48 kHz):
```bash
./build/rpi_multirec --device hw:1,0 --rate 48000 --out /home/pi/capture.rf64
```

Example (96 kHz, larger ring buffer):
```bash
./build/rpi_multirec --device hw:1,0 --rate 96000 --ring-ms 3000 --out /home/pi/capture.rf64
```

## Options
- `--device <name>` ALSA device (default: `default`)
- `--out <path>` output RF64 file (optional when `--mic` is set)
- `--rate 48000|96000`
- `--channels <n>` default 84
- `--mic spcmic|zylia` mic preset for device/channels/access
- `--format s16|s24` sample format (default s24)
- `--access rw|mmap` ALSA access type (default rw)
- `--start auto|explicit` stream start mode (default explicit)
- `--stdin-raw` read raw PCM from stdin instead of ALSA
- `--buffer-ms <ms>` ALSA buffer time (default 200)
- `--period-ms <ms>` ALSA period time (default 50)
- `--ring-ms <ms>` ring buffer time (default 5000)
- `--status-ms <ms>` print status every N ms (default 0=off)
- `--list-devices` list ALSA PCM devices and exit

## Mic presets
`--mic spcmic` defaults:
- `--device hw:CARD=s02E5D5,DEV=0`
- `--channels 84`
- `--access rw`

`--mic zylia` defaults:
- `--device hw:CARD=ZM13E,DEV=0`
- `--channels 19`
- `--access mmap`

Explicit `--device`, `--channels`, and `--access` arguments override the preset values.

## Automatic file naming
If `--out` is not provided, the app auto-generates the filename as:
`<mic>_YYYYMMDD_HHMMSS.rf64`

Examples:
- `spcmic_20260222_143015.rf64`
- `zylia_20260222_143045.rf64`

Auto naming requires `--mic spcmic|zylia`.

## USB stability checklist (sporadic clicks with zero XRUNs)
If you see kernel logs like:
```
dwc2_hc_halt() Channel can't be halted
```
it points to USB controller instability on the Pi.

Recommended steps:
1. Use a known-good 5V/2.5A+ power supply.
2. Try a short, high-quality USB cable.
3. Disable Wi-Fi/Bluetooth temporarily to reduce bus contention:
   - `sudo rfkill block wifi`
   - `sudo rfkill block bluetooth`
4. If available, use a powered USB hub between the Pi and mic.

## Stdin raw mode (arecord + RF64)
Use ALSA `arecord` for capture and pipe raw PCM into the RF64 writer:
```bash
arecord -D hw:CARD=ZM13E,DEV=0 -f S24_3LE -c 19 -r 48000 -t raw | \
  ./build/rpi_multirec --stdin-raw --rate 48000 --channels 19 --format s24 --out /home/pi/zm1.rf64
```
## Rebuild Zylia driver DKMS cleanly
Source location: /usr/src/zm-1-driver-2.7.0/linux/src

sudo dkms remove -m zm-1-driver -v 2.7.0 --all || true
sudo dkms add -m zm-1-driver -v 2.7.0
sudo dkms build -m zm-1-driver -v 2.7.0
sudo dkms install -m zm-1-driver -v 2.7.0
sudo dpkg --configure -a

Load the module:
sudo modprobe zm_1_driver

Verify it loaded:
lsmod | grep zm_1_driver
dmesg | tail -n 50
