# Inkbird IAM-T1 Python Client Guide

## Quick Start

### 1. Install Dependencies

```bash
# Using uv (recommended)
uv sync

# Or using pip
pip install bleak tqdm matplotlib
```

### 2. Find Your Sensor

```bash
python inkbird_client.py --scan
```

Expected output:
```
Scanning for Inkbird devices (15s)...
  Found: XX:XX:XX:XX:XX:XX - Ink@IAM-T1 (RSSI: -45)

✓ Found 1 device(s)
```

### 3. Download History

```bash
python inkbird_client.py
```

This downloads all historical data and displays an interactive chart.

## Features

### ✅ Implemented

- [x] **Auto device discovery** - Scans for Inkbird IAM-T1 sensors
- [x] **Progress indication** - Shows download progress with tqdm
- [x] **Historical data download** - Downloads all stored sensor readings
- [x] **Time reconstruction** - Calculates timestamps from recording intervals
- [x] **Statistics summary** - Shows min/max/average for all sensors
- [x] **Interactive charting** - Single chart with CO2, temperature, humidity
- [x] **Multiple y-axes** - CO2 on left, temp/humidity on right
- [x] **Auto time formatting** - Adapts x-axis format to data range
- [x] **Error handling** - Graceful failures with helpful messages

### 📊 Chart Details

The chart displays three sensors on a single figure:

| Sensor | Y-Axis | Color | Style |
|--------|--------|-------|-------|
| CO₂ | Left (primary) | Blue | Solid line |
| Temperature | Right (secondary) | Red | Dashed line |
| Humidity | Right (secondary) | Green | Dotted line |

#### Interactive Features

- **Zoom**: Click and drag to zoom into a region
- **Pan**: Right-click and drag to pan
- **Reset view**: Click home button in toolbar
- **Save image**: Click save button to export PNG/SVG
- **Grid lines**: Semi-transparent grid for easier reading

#### Time Formatting

The x-axis automatically formats based on data range:

| Duration | Format | Example |
|----------|--------|---------|
| < 1 day | Time only | 14:30 |
| 1-7 days | Date + Time | 01/03 14:30 |
| > 7 days | Date only | 2026-01-03 |

## Command-Line Options

```bash
python inkbird_client.py [OPTIONS]
```

| Option | Description | Default |
|--------|-------------|---------|
| `--scan` | Scan for devices and exit | Download mode |
| `--no-chart` | Skip chart display | Show chart |
| `--timeout SECONDS` | Scan timeout duration | 15 |
| `--help` | Show help message | - |

### Examples

```bash
# Basic usage (scan + download + chart)
python inkbird_client.py

# Scan only
python inkbird_client.py --scan

# Download without chart
python inkbird_client.py --no-chart

# Longer scan timeout (30 seconds)
python inkbird_client.py --timeout 30
```

## Understanding the Output

### Download Progress

```
Downloading history: 100%|████████| 1440/1440 [00:12<00:00, 120.0 rec/s]
```

- **1440/1440**: Downloaded 1440 of 1440 records
- **[00:12<00:00]**: 12 seconds elapsed, 0 seconds remaining
- **120.0 rec/s**: Download speed (records per second)

### Statistics Section

```
STATISTICS
CO₂:         min= 420 ppm, max=1200 ppm, avg= 650.5 ppm
Temperature: min=20.5°C,  max=24.3°C,  avg= 22.1°C
Humidity:    min=35.2%,   max=65.8%,   avg= 48.3%
```

Shows minimum, maximum, and average for each sensor across all readings.

### Time Range

```
Time range: 2026-01-02 14:23:00 to 2026-01-03 14:23:00
Recording interval: 10 minutes
```

- **Time range**: Oldest and newest readings
- **Recording interval**: How often the sensor records data (typically 10 minutes)

## How Time Reconstruction Works

The Inkbird IAM-T1 protocol does **not** include absolute timestamps in historical records. Instead, each record contains:

1. **Recording interval** (typically 10 minutes)
2. **Records in chronological order** (oldest → newest)

The client reconstructs timestamps using this algorithm:

```python
# Use current time as timestamp of last record
last_timestamp = datetime.now()

# Work backwards from last record
for i, record in enumerate(reversed(records)):
    timestamp = last_timestamp - timedelta(minutes=i * interval)
```

### Accuracy

- **✓ Relative timing**: Accurate (based on sensor's recording interval)
- **⚠ Absolute timing**: Approximate (assumes download happens immediately after last reading)

For most use cases, this provides sufficient accuracy (±10 minutes).

## Troubleshooting

### No devices found

**Symptoms:**
```
✗ No Inkbird devices found
```

**Solutions:**
1. Ensure sensor is powered on (check display)
2. Check battery level (replace if low)
3. Move closer to sensor (BLE range ~10m)
4. Increase scan timeout: `--timeout 30`
5. Close Inkbird mobile app (may be holding connection)
6. Reset sensor (remove and reinsert battery)

### Connection fails

**Symptoms:**
```
✗ Connection failed
```

**Solutions:**
1. Sensor may be connected to another device (close mobile app)
2. Disable other Bluetooth connections on computer
3. Reset Bluetooth on computer
4. Reset sensor (remove battery for 10 seconds)

### Download hangs

**Symptoms:**
- Progress bar stuck at 0%
- No records received after 30+ seconds

**Solutions:**
1. Wait up to 2 minutes (large datasets take time)
2. Sensor memory may be empty (no history to download)
3. Reset connection and try again
4. Check sensor battery level

### Chart doesn't appear

**Symptoms:**
- Script completes but no chart window

**Solutions:**
1. Run with `--no-chart` to verify download works
2. Check matplotlib backend: `export MPLBACKEND=TkAgg` (macOS/Linux)
3. Update matplotlib: `pip install --upgrade matplotlib`

### Permission denied (macOS)

**Symptoms:**
```
PermissionError: [Errno 1] Operation not permitted
```

**Solutions:**
1. Grant Bluetooth permission to Terminal/Python:
   - System Settings → Privacy & Security → Bluetooth
   - Add Terminal/Python
2. Run from terminal with Bluetooth permission

## Demo Mode

Test the charting functionality without a sensor:

```bash
python demo_inkbird_client.py
```

This generates 24 hours of synthetic sensor data with realistic patterns:
- CO₂ varies throughout day (higher during daytime)
- Temperature peaks in afternoon
- Humidity inversely correlates with temperature
- Atmospheric pressure slowly drifts

Perfect for:
- Testing visualization code
- Previewing chart appearance
- Demonstrating to others without hardware

## Technical Details

### Protocol Implementation

Based on reverse-engineered Inkbird Android APK:

1. **Service UUID**: `0000FFE0-...-00805f9b34fb`
2. **Notify UUID**: `0000FFE4-...-00805f9b34fb` (receive data)
3. **Write UUID**: `0000FFE9-...-00805f9b34fb` (send commands)

### History Download Protocol

1. **Request**: Send `55AA0706000C` to Write UUID
2. **Response 1**: 2 bytes = record count (big-endian)
3. **Response N**: 10-byte records (CO2, temp, humidity, pressure, interval)
4. **End**: `6666` end marker

### Record Format (10 bytes)

| Bytes | Field | Encoding |
|-------|-------|----------|
| 0-1 | CO₂ | Big-endian (ppm) |
| 2 | Unit & Sign | Lower nibble: 0=°C/1=°F, Upper nibble: 0=+/1=- |
| 3-4 | Temperature | Big-endian × 10 (0.1°C resolution) |
| 5-6 | Humidity | Big-endian × 10 (0.1% resolution) |
| 7-8 | Pressure | Big-endian (hPa) |
| 9 | Interval | Recording interval (minutes) |

See [INKBIRD_IAM_T1_PROTOCOL.md](INKBIRD_IAM_T1_PROTOCOL.md) for full protocol documentation.

## Limitations

1. **No partial downloads**: Must download entire history each time
2. **No record deletion**: Cannot clear sensor memory via BLE
3. **No real-time streaming**: For real-time data, use `test_inkbird.py`
4. **Single connection**: Sensor can only connect to one device at a time
5. **No data export**: Currently no CSV/JSON export (charts only)

## Future Enhancements

Potential improvements:

- [ ] Export to CSV/JSON
- [ ] SQLite database storage
- [ ] Real-time monitoring mode
- [ ] Multiple sensor support
- [ ] Web dashboard
- [ ] Configurable chart styles
- [ ] Statistical analysis (trends, anomalies)
- [ ] Alert thresholds

## Related Files

| File | Purpose |
|------|---------|
| `inkbird_client.py` | Main BLE client (this implementation) |
| `demo_inkbird_client.py` | Demo with synthetic data |
| `test_inkbird.py` | Real-time data test |
| `INKBIRD_IAM_T1_PROTOCOL.md` | Protocol documentation |
| `README.md` | Project overview |

## License

[Your license here]
