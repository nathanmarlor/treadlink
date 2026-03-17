# TreadLink

ESP32-S3 firmware that bridges a BLE treadmill (FTMS) to a Garmin watch by presenting as a Running Speed and Cadence (RSC) footpod sensor.

Built for the [Seeed Studio XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html).

## Install

Flash directly from your browser — no tools required:

**[Install TreadLink](https://nathanmarlor.github.io/treadlink/)** (Chrome/Edge, USB-C cable)

Or download the binaries from the [latest release](https://github.com/nathanmarlor/treadlink/releases/latest).

## What it does

Most Garmin watches don't natively support BLE treadmill data (FTMS). TreadLink sits between your treadmill and watch:

```
Treadmill ──BLE FTMS──> TreadLink ──BLE RSC──> Garmin Watch
                            │
                        WiFi Web UI
```

- Connects to any BLE FTMS treadmill as a client
- Advertises as a footpod sensor that Garmin watches discover natively
- Converts speed, cadence, distance, and incline data in real time
- Provides a web UI for setup, monitoring, and treadmill control

## Features

- **Dual-role BLE** — simultaneous GATT client (treadmill) and GATT server (Garmin)
- **Auto-reconnect** — reconnects to saved treadmill with exponential backoff
- **Treadmill control** — set speed, incline, start/stop via FTMS Control Point from the web UI
- **WiFi web UI** — responsive single-page interface for configuration and live data
- **Speed unit support** — km/h and mph throughout
- **Simulated data** — diagnostics mode to test Garmin integration without a treadmill
- **Status LED** — blink patterns indicate connection state
- **Persistent config** — NVS storage for treadmill address, WiFi credentials, cadence settings

## Hardware

- Seeed Studio XIAO ESP32-S3
- USB-C cable for power and initial flashing
- Optional: 3D printed case (see [`case/`](case/))

## 3D Printed Case

A printable enclosure is provided in the `case/` directory:

- **`TreadLink.3mf`** — ready-to-print 3MF file for the XIAO ESP32-S3

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Flash
pio run -t upload

# Monitor serial output
pio device monitor
```

## Setup

1. Flash firmware via the [web installer](https://nathanmarlor.github.io/treadlink/) or PlatformIO
2. Connect to the **TreadLink** WiFi AP (password: `treadlink`)
3. Browse to `192.168.4.1`
4. Scan for your treadmill and connect
5. On your Garmin watch: Settings > Sensors > Add > search for "TreadLink"
6. Start a treadmill run on Garmin — speed and cadence data will flow automatically

To use your home WiFi instead of AP mode, configure WiFi STA credentials in the web UI settings.

## Web UI

The web interface provides:

- **Live data** — speed, cadence, distance, incline updated in real time
- **Treadmill scan & connect** — discover and pair BLE FTMS treadmills
- **Treadmill control** — speed/incline sliders, presets, start/stop (when supported)
- **Configuration** — WiFi mode, speed units, cadence estimation parameters
- **Simulate** — test Garmin integration without a treadmill
- **Log viewer** — real-time event log for debugging

## Architecture

```
components/
├── ble_common/          # Shared NimBLE stack init
├── ble_ftms_client/     # GATT client — connects to treadmill
├── ble_rsc_server/      # GATT server — advertises as footpod
├── data_bridge/         # FTMS → RSC unit conversion + cadence estimation
├── config_store/        # NVS persistent configuration
├── wifi_manager/        # WiFi AP/STA management
├── web_server/          # HTTP REST API + embedded HTML UI
└── led_status/          # Status LED blink patterns
main/
└── main.c               # Orchestration + callbacks
```

## LED Status

| Pattern | Meaning |
|---------|---------|
| Slow blink (1s) | No connection |
| Fast blink (100ms) | Scanning / connecting |
| Double blink | Treadmill connected, waiting for Garmin |
| Solid on | Both connected — data flowing |

## License

MIT
