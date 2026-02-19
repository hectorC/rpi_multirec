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
- `--out <path>` output RF64 file
- `--rate 48000|96000`
- `--channels <n>` default 84
- `--buffer-ms <ms>` ALSA buffer time (default 200)
- `--period-ms <ms>` ALSA period time (default 50)
- `--ring-ms <ms>` ring buffer time (default 2000)
- `--status-ms <ms>` print status every N ms (default 0=off)
- `--list-devices` list ALSA PCM devices and exit

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
