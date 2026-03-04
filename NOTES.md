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
sudo apt install -y build-essential cmake pkg-config libasound2-dev libsndfile1-dev libgpiod-dev
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
- `--hat-ui` enable Waveshare 1.3inch LCD HAT status UI
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

## Waveshare 1.3inch LCD HAT
The app supports the Waveshare SPI HAT with `--hat-ui`.

Current behavior:
- App starts in `IDLE` when `--hat-ui` is enabled.
- `KEY2` starts recording.
- `KEY1` stops the current take and returns to `IDLE` (multi-take session).
- `KEY3` toggles LCD backlight.
- Joystick `LEFT/RIGHT` in `IDLE` selects `spcmic`/`zylia` preset for the next take.
- Joystick `UP/DOWN` in `IDLE` selects `96 kHz`/`48 kHz`.
- If the selected mic is not connected, the mic label is shown in red and recording start is disabled.
- After `KEY1` stop, a new take cannot start until buffered audio is fully written and the file is closed.
- During stop/finalize, elapsed time is held and shown in red; it resets to zero only after finalize completes.
- Display shows recording state, elapsed time, mic preset/custom, rate/channels, XRUNs, and dropped MB.
- Multiple takes are supported in one app run.
- With auto naming, each take gets a fresh `<mic>_YYYYMMDD_HHMMSS.rf64` file.
- If `--out` is provided, take 1 uses that filename and take 2+ use `_takeNNN` suffixes.

Linux requirements:
- SPI enabled (`/dev/spidev0.0` must exist).
- GPIO and SPI device access permissions (`libgpiod` + `/dev/spidev0.0`).
- If permissions block access, run with `sudo`.

Example:
```bash
./build/rpi_multirec --mic spcmic --rate 48000 --hat-ui --status-ms 1000
```

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

## Auto-start on boot (systemd)
Create service file:
`/etc/systemd/system/rpi_multirec.service`

```ini
[Unit]
Description=RPi Multi Recorder
After=systemd-udev-settle.service sound.target
Wants=systemd-udev-settle.service

[Service]
Type=simple
WorkingDirectory=/home/hcenteno/rpi_multirec
ExecStart=/home/hcenteno/rpi_multirec/build/rpi_multirec --hat-ui --mic spcmic
Restart=on-failure
RestartSec=2
User=root
Group=root
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Commands to apply:
```bash
sudo systemctl daemon-reload
sudo systemctl enable rpi_multirec.service
sudo systemctl start rpi_multirec.service
sudo systemctl status rpi_multirec.service
journalctl -u rpi_multirec.service -f
```

Reboot test:
```bash
sudo reboot
```
