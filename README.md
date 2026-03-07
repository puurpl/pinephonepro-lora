# LoRa on PinePhone Pro (Mobian)

Running LoRa on a PinePhone Pro using the **PineDio LoRa backplate** and
**Mobian** (Debian trixie, Megi kernel 6.19+).

This repo contains everything needed to:

1. **Run Meshtastic** — off-grid encrypted mesh messaging via `meshtasticd`
   *(confirmed working: TX and RX tested with Heltec V2)*
2. **Run Helium IoT / LoRaWAN** — send sensor data to the internet via Helium hotspots
   *(built and compiles, but not yet tested end-to-end with a live network)*

Both use the same hardware: the PineDio LoRa backplate's SX1262 radio,
accessed through its ATtiny84 I2C-to-SPI bridge.

## Hardware

| Component | Detail |
|---|---|
| Phone | PinePhone Pro |
| OS | Mobian (Debian 13 / trixie) |
| Kernel | Megi 6.19+ |
| LoRa radio | Semtech SX1262 (sub-GHz, 915 MHz ISM band) |
| Bridge MCU | ATtiny84 (I2C-to-SPI bridge at address 0x28) |
| I2C bus | `/dev/i2c-5` (`ff140000.i2c`, pogo pin bus) |
| Antenna | Flex PCB antenna bonded to back cover (~1-2 dBi) |
| Range | 0.5-2 km urban (limited by antenna) |

### How it works

```
Phone (Linux)
  └── /dev/i2c-5  ──pogo pins──  ATtiny84 (0x28)  ──SPI──  SX1262 radio  ──RF──  air
```

The PineDio LoRa backplate sits under the phone's back cover, connecting via
pogo pins. The ATtiny84 acts as a transparent I2C-to-SPI bridge: you write
SPI commands over I2C, and it forwards them to the SX1262 radio chip.

## Prerequisites

- PinePhone Pro with PineDio LoRa backplate installed
- Mobian (Debian 13 / trixie) with Megi kernel 6.19+
- Back cover firmly seated (pogo pin contact is intermittent)
- User in the `i2c` group: `sudo usermod -aG i2c $USER`

### Verify hardware

```bash
# Check the ATtiny84 bridge responds on I2C bus 5
sudo i2cdetect -y 5
# Look for "28" in the output — that's the LoRa bridge
```

If `28` doesn't appear, reseat the back cover firmly and try again.

## Part 1: Meshtastic (Mesh Messaging)

Meshtastic provides off-grid encrypted text messaging over LoRa. Messages hop
through intermediate nodes — no cell towers or internet needed.

### 1.1 Install dependencies

```bash
sudo apt update
sudo apt install cmake libi2c-dev python3-pip python3-venv libgpiod-dev \
    libyaml-cpp-dev libbluetooth-dev libuv1-dev libusb-1.0-0-dev \
    build-essential g++ git
```

### 1.2 Clone and build meshtastic firmware

```bash
git clone --depth 1 https://github.com/meshtastic/firmware.git ~/meshtastic-firmware
cd ~/meshtastic-firmware
python3 -m venv venv && source venv/bin/activate
pip install platformio
pio pkg install --environment native
```

### 1.3 Apply I2C bridge patches

The meshtastic firmware doesn't natively support I2C-attached radios. We add
an `I2CBridgeHal` — a custom RadioLib HAL that proxies SPI commands over I2C
to the ATtiny84 bridge.

```bash
# Copy the HAL file
cp /path/to/this/repo/meshtastic/I2CBridgeHal.h \
    ~/meshtastic-firmware/src/platform/portduino/

# Apply source code modifications (see meshtastic/PATCHING.md for details)
```

**See [`meshtastic/PATCHING.md`](meshtastic/PATCHING.md)** for the exact
changes to `PortduinoGlue.h`, `PortduinoGlue.cpp`, `RadioInterface.cpp`,
and `main.cpp`.

### 1.4 Build

```bash
cd ~/meshtastic-firmware
source venv/bin/activate
pio run --environment native    # ~15-18 minutes on PinePhone Pro
```

Binary: `~/.pio/build/native/meshtasticd`

### 1.5 Install config and service

```bash
# Create config directory
sudo mkdir -p /etc/meshtasticd/config.d

# Copy the LoRa config
sudo cp /path/to/this/repo/meshtastic/lora-pinedio-i2c-backplate-sx1262.yaml \
    /etc/meshtasticd/config.d/

# Install the systemd service
sudo cp /path/to/this/repo/meshtastic/meshtasticd.service \
    /etc/systemd/system/
sudo systemctl daemon-reload
```

### 1.6 Start and configure

```bash
# Start the daemon
sudo systemctl start meshtasticd

# Check it's running
sudo systemctl status meshtasticd

# Install the Meshtastic CLI
pip3 install --user --break-system-packages meshtastic

# Set region (US covers USA/Canada/Mexico, 902-928 MHz)
meshtastic --host 127.0.0.1 --set lora.region US

# Check node info
meshtastic --host 127.0.0.1 --info

# Enable on boot
sudo systemctl enable meshtasticd
```

### 1.7 Use Meshtastic

**CLI:**
```bash
meshtastic --host 127.0.0.1 --sendtext "Hello from PinePhone Pro"
meshtastic --host 127.0.0.1 --nodes     # list nearby nodes
```

**GTK client (mobile-optimized GUI):**
```bash
sudo apt install gtk-meshtastic-client
# Connect to TCP 127.0.0.1 (port 4403)
```

### Meshtastic quick reference

```bash
sudo systemctl start meshtasticd       # start
sudo systemctl stop meshtasticd        # stop (ALWAYS use this, never kill -9)
sudo systemctl restart meshtasticd     # restart
sudo journalctl -u meshtasticd -f      # follow logs
meshtastic --host 127.0.0.1 --info     # node info
meshtastic --host 127.0.0.1 --nodes    # list heard nodes
```

## Part 2: Helium IoT / LoRaWAN (experimental)

> **Status:** This tool compiles and builds successfully but has **not been
> tested end-to-end** with a live Helium network. OTAA join, uplink, and
> downlink are implemented but unverified. You will need Helium hotspot
> coverage and a registered LoRaWAN device to test.

A standalone CLI tool that sends data to the internet via the Helium IoT
network using LoRaWAN. Separate from Meshtastic — uses standardized LoRaWAN
protocol instead of Meshtastic's custom mesh protocol.

**Use case:** Send small sensor data packets (temperature, GPS, status) to
the internet via Helium's network of hotspots.

### 2.1 Prerequisites

- Meshtastic must be built first (the tool reuses RadioLib from the meshtastic tree)
- Helium hotspot coverage in your area ([check coverage](https://explorer.helium.com))
- A LoRaWAN Network Server account (Helium Console or ChirpStack)

### 2.2 Configure credentials

```bash
cd /path/to/this/repo/helium-lorawan
cp credentials.h.example credentials.h
nano credentials.h
# Fill in LORAWAN_JOIN_EUI, LORAWAN_DEV_EUI, LORAWAN_APP_KEY, LORAWAN_NWK_KEY
# from your Helium Console device registration
```

### 2.3 Build

```bash
cd /path/to/this/repo/helium-lorawan
make    # ~30 seconds, produces 240KB binary
```

### 2.4 Use

```bash
# Join the Helium network (OTAA — run once)
./helium-lorawan join

# Send data
./helium-lorawan send 1 48656C6C6F           # hex bytes on port 1
./helium-lorawan send 2 "Hello World"        # ASCII on port 2
./helium-lorawan send "temperature=22.5"     # default port 1

# Check session status
./helium-lorawan status
```

The tool automatically stops `meshtasticd` before radio access and restarts it
after. Session state (keys, frame counters) is saved to `~/.helium-lorawan/`
so you don't need to re-join every time.

## Architecture

### I2CBridgeHal

The core of both Meshtastic and Helium support is the `I2CBridgeHal` class —
a custom `RadioLibHal` subclass that translates RadioLib's SPI calls into I2C
transactions for the ATtiny84 bridge.

```
Application (meshtasticd or helium-lorawan)
  └── RadioLib (SX1262 driver, LoRa/LoRaWAN protocols)
        └── I2CBridgeHal (SPI-over-I2C translation)
              └── /dev/i2c-5 → ATtiny84 (0x28) → SX1262 radio
```

Two versions exist:
- **`meshtastic/I2CBridgeHal.h`** — Full version with libgpiod support,
  adaptive IRQ polling, mutex for thread safety, meshtastic logging
- **`helium-lorawan/I2CBridgeHal.h`** — Standalone version with no
  meshtastic/libgpiod dependencies

### I2C bridge protocol

| Operation | I2C Transaction |
|---|---|
| SPI write+read | Write `[0x01, spi_bytes...]` to 0x28, then read N bytes back |
| Standby (idle) | Write `[0x01, 0x80, 0x00]` to 0x28 |

### IRQ handling

The PinePhone Pro's pogo interrupt pin (GPIO3_A0, pin 96) **cannot** be used
as a hardware GPIO interrupt — it conflicts with the I2C5 bus pinctrl group in
the kernel DTS. Requesting it via libgpiod breaks the entire I2C bus.

Instead, the meshtastic HAL uses **software IRQ polling**: a background thread
reads the SX1262's IRQ status register every 100ms via I2C. After detecting
activity, it switches to 15ms fast-polling for ~150ms to catch back-to-back
packets.

## Important warnings

### Never use `kill -9` on meshtasticd

If meshtasticd is killed with SIGKILL mid-I2C-transaction, the ATtiny84's USI
state machine hangs permanently. The LoRa bridge stops responding and the
**only fix is a full power-off** (not reboot — cold shutdown, wait 10 seconds,
power on). Always use `systemctl stop meshtasticd`.

### Pogo pin contact

The pogo pin connection between the phone and backplate is unreliable. If
`i2cdetect -y 5` doesn't show `28`, firmly reseat the back cover. After a
power cycle, always verify the bridge is responding before starting meshtasticd.

### Radio sharing

Only one program can use the SX1262 at a time. The helium-lorawan tool
handles this automatically (stops/restarts meshtasticd). Don't run both
simultaneously.

## Troubleshooting

| Problem | Solution |
|---|---|
| `i2cdetect` shows no `28` | Reseat back cover firmly. If still missing, power off completely (not reboot), wait 10 sec, power on |
| meshtasticd hangs after "Buffer sync OK" | Rebuild the binary (`pio run -e native`) |
| No RX (can't see other nodes) | Verify IRQ polling is working: check logs for "IRQ poll thread started" |
| Port 4403 in use but service stopped | Kill zombie process: `ps aux \| grep meshtasti`, then `kill <pid>` |
| I2C bus completely dead | ATtiny84 USI hung. Full power-off (cold shutdown), wait 10 sec |
| helium-lorawan join fails | Check credentials, Helium hotspot coverage, antenna connection |

## File structure

```
pinephone-lora/
├── README.md                          This file
├── meshtastic/
│   ├── I2CBridgeHal.h                 RadioLibHal for I2C bridge (meshtastic version)
│   ├── PATCHING.md                    How to patch meshtastic firmware source
│   ├── lora-pinedio-i2c-backplate-sx1262.yaml   meshtasticd config
│   └── meshtasticd.service            systemd service file
├── helium-lorawan/
│   ├── I2CBridgeHal.h                 RadioLibHal for I2C bridge (standalone version)
│   ├── main.cpp                       Helium LoRaWAN CLI tool
│   ├── Makefile                       Build system
│   └── credentials.h.example          Template for LoRaWAN credentials
└── docs/
    └── full-build-log.md              Detailed step-by-step build log with analysis
```

## References

- [JF's PineDio driver](https://codeberg.org/JF002/pinedio-lora-driver) — original I2C bridge driver
- [JF's blog post](https://codingfield.com/blog/2021-11/a-driver-for-the-pinephone-lora-backplate/) — reverse-engineering the backplate
- [lupyuen's I2C protocol docs](https://github.com/lupyuen/pinephone-lora) — ATtiny84 bridge protocol
- [Pine64 PineDio docs](https://pine64.org/documentation/Pinedio/Datasheets_and_schematics/)
- [Meshtastic firmware](https://github.com/meshtastic/firmware) — source code
- [Meshtastic Linux-native docs](https://meshtastic.org/docs/hardware/devices/linux-native-hardware/)
- [RadioLib](https://github.com/jgromes/RadioLib) — radio abstraction library
- [Helium IoT](https://docs.helium.com/) — LoRaWAN network documentation
