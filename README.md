# CO2 Display Project

ESP32-based CO2 sensor display with BLE connectivity to Inkbird IAM-T1 sensors.

## Project Structure

```
co2_display/
├── src/                    # ESP32 firmware (C)
│   ├── main.c             # Main application
│   ├── ble/               # BLE client for Inkbird sensor
│   ├── drivers/           # E-paper display drivers
│   ├── ui/                # UI rendering
│   └── data/              # Data management
├── inkbird_client.py      # Python BLE client for data download
├── test_inkbird.py        # Python BLE connection test
└── platformio.ini         # Build configuration
```

## Hardware

- **MCU**: ESP32-C3 (ESP32-C3-DevKitM-1)
- **Display**: E-paper display (SPI)
- **Sensor**: Inkbird IAM-T1 (BLE)

## Firmware (ESP32)

### Prerequisites

- [PlatformIO](https://platformio.org/)
- ESP-IDF v5.5.0 (via PlatformIO)

### Build & Flash

```bash
# Build
pio run

# Build and upload
pio run -t upload

# Build, upload, and monitor
pio run -t upload && pio device monitor

# Clean build
pio run -t clean
```

### Configuration

```bash
# Open ESP-IDF menuconfig
pio run -t menuconfig
```

See [AGENTS.md](AGENTS.md) for detailed build instructions and coding guidelines.

## Python Client (Inkbird IAM-T1)

The `inkbird_client.py` script downloads historical data from Inkbird IAM-T1 CO2 sensors via Bluetooth Low Energy (BLE).

### Features

- **Device scanning**: Auto-discover Inkbird IAM-T1 sensors
- **History download**: Download all stored sensor readings with progress bar
- **Data visualization**: Interactive charts showing CO2, temperature, and humidity
- **Time reconstruction**: Calculates timestamps based on recording intervals

### Prerequisites

Python 3.13+ with dependencies:

```bash
# Install dependencies (using uv)
uv sync

# Or with pip
pip install bleak tqdm matplotlib
```

### Usage

#### Scan for devices

```bash
python inkbird_client.py --scan
```

Output:
```
Scanning for Inkbird devices (15s)...
  Found: XX:XX:XX:XX:XX:XX - Ink@IAM-T1 (RSSI: -45)

✓ Found 1 device(s)
```

#### Download history and display chart

```bash
python inkbird_client.py
```

This will:
1. Scan for Inkbird devices
2. Connect to the first device found
3. Download historical data with progress bar
4. Display statistics
5. Show interactive chart with CO2, temperature, and humidity

Output:
```
Scanning for Inkbird devices (15s)...
  Found: XX:XX:XX:XX:XX:XX - Ink@IAM-T1 (RSSI: -45)

Using device: XX:XX:XX:XX:XX:XX - Ink@IAM-T1
Connecting to XX:XX:XX:XX:XX:XX (Ink@IAM-T1)...
  ✓ Connected
Pairing with sensor...
  ✓ Pair command sent

Starting history download...
Downloading history: 100%|████████| 1440/1440 [00:12<00:00, 120.0 rec/s]

✓ Downloaded 1440 readings
  Time range: 2026-01-02 14:23:00 to 2026-01-03 14:23:00
  Recording interval: 10 minutes

============================================================
STATISTICS
============================================================
CO₂:         min= 420 ppm, max=1200 ppm, avg= 650.5 ppm
Temperature: min=20.5°C,  max=24.3°C,  avg= 22.1°C
Humidity:    min=35.2%,   max=65.8%,   avg= 48.3%

============================================================
SAMPLE READINGS
============================================================
First reading:
  2026-01-02 14:23:00: CO2=450ppm, T=21.2°C, H=45.3%, P=1013hPa

Last reading:
  2026-01-03 14:23:00: CO2=680ppm, T=22.8°C, H=52.1%, P=1015hPa

Displaying chart...
```

#### Download without chart

```bash
python inkbird_client.py --no-chart
```

#### Custom scan timeout

```bash
python inkbird_client.py --timeout 30
```

### Chart Features

The generated chart displays:

- **Primary Y-axis (left)**: CO₂ concentration in PPM (blue solid line)
- **Secondary Y-axis (right)**: 
  - Temperature in °C (red dashed line)
  - Relative humidity in % (green dotted line)
- **X-axis**: Timestamp (auto-formatted based on data range)
- **Grid**: Semi-transparent grid for easier reading
- **Legend**: Top-left corner

The chart is interactive:
- Zoom: Click and drag
- Pan: Right-click and drag
- Reset: Home button in toolbar
- Save: Save button in toolbar

### Protocol Documentation

See [INKBIRD_IAM_T1_PROTOCOL.md](INKBIRD_IAM_T1_PROTOCOL.md) for detailed protocol documentation.

## Testing

### Python Tests

```bash
# Test BLE connection and real-time data
python test_inkbird.py
```

### ESP32 Tests

```bash
# Run all unit tests
pio test

# Run specific test
pio test -f test_<name>

# Static analysis
pio check
```

## Troubleshooting

### Python Client

**No devices found:**
- Ensure Inkbird IAM-T1 is powered on
- Check battery level
- Move closer to sensor (BLE range ~10m)
- Try increasing scan timeout: `--timeout 30`

**Connection fails:**
- Reset the sensor (remove and reinsert battery)
- Disable other Bluetooth connections on your computer
- Close the Inkbird mobile app if running

**Download hangs:**
- Wait up to 2 minutes (large datasets take time)
- Sensor may be in use by another device
- Try reconnecting

### ESP32 Firmware

**Build fails:**
```bash
pio run -t fullclean
pio run
```

**Upload fails:**
- Check USB connection
- Hold BOOT button while uploading
- Check correct USB port in `platformio.ini`

**Serial monitor garbled:**
- Ensure baud rate is 115200
- Reset ESP32 after upload

## License

[Add your license here]

## Credits

- Inkbird IAM-T1 protocol reverse-engineered from Android APK
- ESP-IDF framework by Espressif
- BLE communication via Bleak library
