# Installation

These steps target a fresh Raspberry Pi OS Lite installation.

This document is intentionally narrower than `DEVNOTES.md`. `DEVNOTES.md` remains the full working notebook. `INSTALL.md` is the cleaner path for setting up a usable recorder from a vanilla system.

## Scope

The installer and instructions in this repository currently cover:

- building `rpi_multirec`
- creating the internal recordings directory
- optional boot-time startup with `systemd`
- optional Samba + Avahi setup for file transfer
- optional USB exFAT automount on Raspberry Pi OS Lite
- enabling SPI and I2C when `raspi-config` is available

They do not download third-party vendor drivers for you.

## Target OS

Recommended target:

- Raspberry Pi OS Lite

The current installer is written for Raspberry Pi OS Lite first. Some notes in `DEVNOTES.md` reference earlier Ubuntu Server bring-up work; treat those as historical reference unless you are intentionally using Ubuntu again.

## Before you start

- Flash a fresh Raspberry Pi OS Lite image.
- Boot the Pi and connect it to the network.
- Update the system once:

```bash
sudo apt update
sudo apt full-upgrade -y
sudo reboot
```

- Install `git` if needed:

```bash
sudo apt install -y git
```

- Clone this repository:

```bash
git clone <repo-url> ~/rpi_multirec
cd ~/rpi_multirec
```

## Zylia note

If you are using the Zylia microphone on Raspberry Pi OS Lite, install the official Zylia driver package from Zylia first.

Current project status:

- On Raspberry Pi OS Lite, the vendor `.deb` installs cleanly.
- The DKMS source rebuild steps in `DEVNOTES.md` were only needed during earlier Ubuntu Server work and are not part of the normal Raspberry Pi OS Lite installation path.

After installing the vendor package, verify that ALSA sees the device:

```bash
arecord -l
```

## Quick install

Run the provisioning script as root from the repository:

```bash
cd ~/rpi_multirec
sudo ./scripts/install_rpi.sh
```

Default behavior:

- installs build/runtime dependencies
- enables SPI and I2C via `raspi-config` when available
- creates `/srv/rpi_multirec/recordings`
- installs Samba + Avahi configuration
- installs USB exFAT automount support
- builds the recorder
- installs the boot service but does not force-start it unless you choose to start it yourself

## Common installer options

```bash
sudo ./scripts/install_rpi.sh --help
```

Useful examples:

```bash
sudo ./scripts/install_rpi.sh --repo-dir /home/pi/rpi_multirec --run-user pi
sudo ./scripts/install_rpi.sh --no-samba --no-automount
sudo ./scripts/install_rpi.sh --recorder-args "--hat-ui --mic zylia"
sudo ./scripts/install_rpi.sh --start-service
```

## What the installer changes

### Packages

The script installs the packages needed for build and normal operation from the Raspberry Pi OS repositories.

Core packages:

- `build-essential`
- `cmake`
- `pkg-config`
- `libasound2-dev`
- `libsndfile1-dev`
- `libgpiod-dev`
- `i2c-tools`

Additional packages depend on the enabled installer features:

- Samba / Avahi: `samba`, `avahi-daemon`, `avahi-utils`, `smbclient`, `libnss-mdns`
- USB automount: `udisks2`, `exfatprogs`

### SPI / I2C

If `raspi-config` is available, the script runs:

- `raspi-config nonint do_spi 0`
- `raspi-config nonint do_i2c 0`

That is the intended path for the Waveshare LCD HAT and UPS HAT.

### Recordings directory

The script creates:

```text
/srv/rpi_multirec/recordings
```

### Samba / Avahi

If Samba setup is enabled, the script:

- creates the `recshare` system user if needed
- installs the managed `[recordings]` Samba share block
- enables `smbd` and `avahi-daemon`
- prompts for a Samba password only if the Samba account does not already exist

Windows access:

```text
\\rpirec.local\recordings
```

macOS Finder access:

```text
smb://rpirec.local/recordings
```

### USB automount

If automount is enabled, the script installs:

- `deploy/udev/99-rpi_multirec-automount.rules`
- `deploy/systemd/rpi-usb-automount@.service`

This allows Raspberry Pi OS Lite to mount arbitrary exFAT USB storage so the recorder can use it at startup.

### Boot service

If boot service setup is enabled, the script renders:

```text
/etc/systemd/system/rpi_multirec.service
```

from the template in:

```text
deploy/systemd/rpi_multirec.service.in
```

## Post-install checks

Run:

```bash
./scripts/postinstall_check.sh
```

This verifies the most important pieces of the setup:

- built binary exists
- recordings directory exists
- SPI and I2C device nodes are present
- Samba / Avahi / automount services are visible
- ALSA devices can be listed

## Start the recorder service

If you enabled the boot service and want to start it immediately:

```bash
sudo systemctl start rpi_multirec.service
sudo systemctl status rpi_multirec.service --no-pager
```

To follow logs:

```bash
journalctl -u rpi_multirec.service -f
```

## Re-running the installer

The script is intended to be re-runnable. It will:

- reuse existing users/directories where possible
- replace managed config blocks/templates
- keep a backup of `/etc/samba/smb.conf` before editing it

## What still belongs in DEVNOTES.md

`DEVNOTES.md` remains the place for:

- bring-up notes
- experimental commands
- Ubuntu-specific historical notes
- one-off troubleshooting details
- anything not yet stable enough for the formal installation path
