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
- `--list-devices` list ALSA PCM devices and exit
