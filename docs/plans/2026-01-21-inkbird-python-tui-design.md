# Inkbird IAM-T1 Python TUI Application Design

## Overview

A cross-platform terminal user interface (TUI) application for monitoring Inkbird IAM-T1 CO2 sensors via Bluetooth Low Energy. Replicates functionality of the existing C/ESP32 implementation in a desktop Python application.

## Technology Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| UI Framework | Textual | Modern async TUI, rich widgets, cross-platform |
| BLE Library | Bleak | Async, cross-platform (macOS/Linux/Windows) |
| Storage | SQLite | Simple, file-based, good for time-series queries |
| Charts | plotext | ASCII/Unicode charts, Textual-compatible |
| Multi-sensor | 1-4 sensors | Matches C implementation capability |
| Settings | Full read/write | Complete sensor configuration control |

## Project Structure

```
inkbird_tui/
├── main.py                 # Entry point, Textual app
├── ble/
│   ├── __init__.py
│   ├── protocol.py         # Packet building/parsing
│   ├── scanner.py          # Device discovery
│   └── client.py           # BLE connection manager (Bleak async)
├── data/
│   ├── __init__.py
│   ├── models.py           # Data classes (Reading, HistoryRecord, Settings)
│   └── storage.py          # SQLite persistence
├── ui/
│   ├── __init__.py
│   ├── app.py              # Main Textual App class
│   ├── screens/
│   │   ├── main.py         # Dashboard with live readings
│   │   ├── history.py      # Historical chart view
│   │   └── settings.py     # Sensor configuration
│   └── widgets/
│       ├── sensor_tile.py  # Individual sensor display
│       ├── chart.py        # Plotext wrapper
│       └── status_bar.py   # Connection status
└── config.py               # App configuration
```

## BLE Protocol Layer

### UUIDs (from INKBIRD_IAM_T1_PROTOCOL.md)

```python
SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"  # Receive data
WRITE_UUID = "0000ffe9-0000-1000-8000-00805f9b34fb"   # Send commands
```

### Command IDs

```python
CMD_REALTIME = 0x09      # Request live data
CMD_PAIRING = 0x08       # Initiate pairing
CMD_CO2_SETTINGS = 0x02  # CO2 mode/calibration
CMD_THRESHOLDS = 0x03    # CO2 thresholds
CMD_ALARM = 0x04         # Alarm settings
CMD_CALIBRATION = 0x05   # Offset calibration
CMD_HISTORY = 0x07       # History download
```

### Packet Structure

All commands: `[0x55, 0xAA, cmd_id, length, ...data, checksum]`

Checksum: `sum(all_bytes) & 0xFF`

### Data Classes

- `SensorReading` - CO2, temp, humidity, pressure, timestamp
- `HistoryRecord` - Same fields plus interval_mins
- `CO2Settings` - display_mode, use_custom, auto_calibration, manual_mode, manual_cal_value
- `AlarmSettings` - enabled, alarm_mode, alarm_value
- `CalibrationSettings` - co2_offset, temp_offset, hum_offset, use_fahrenheit

## Data Storage Layer

### SQLite Schema

```sql
-- Sensor registry (persists discovered sensors)
CREATE TABLE sensors (
    mac TEXT PRIMARY KEY,
    name TEXT,
    addr_type INTEGER,
    last_seen INTEGER,
    enabled INTEGER DEFAULT 1
);

-- Real-time readings (rolling buffer, keep last 24h per sensor)
CREATE TABLE readings (
    id INTEGER PRIMARY KEY,
    mac TEXT,
    timestamp INTEGER,
    co2_ppm INTEGER,
    temperature INTEGER,  -- 0.1°C units
    humidity INTEGER,     -- 0.1% units
    pressure INTEGER,
    FOREIGN KEY (mac) REFERENCES sensors(mac)
);
CREATE INDEX idx_readings_time ON readings(mac, timestamp);

-- Historical data downloaded from sensor
CREATE TABLE history (
    id INTEGER PRIMARY KEY,
    mac TEXT,
    download_time INTEGER,
    record_index INTEGER,
    co2_ppm INTEGER,
    temperature INTEGER,
    humidity INTEGER,
    pressure INTEGER,
    interval_mins INTEGER,
    FOREIGN KEY (mac) REFERENCES sensors(mac)
);
```

### Storage Location

`~/.inkbird/data.db` (created automatically)

### Storage API

- `save_reading(mac, reading)` - Insert with auto-cleanup of old data
- `get_readings(mac, since_timestamp)` - Query for charts
- `save_history_batch(mac, records)` - Bulk insert after download
- `get_history(mac, limit)` - Fetch for history view
- `save_sensor(mac, name, addr_type)` - Persist discovered sensor
- `get_known_sensors()` - Load on startup

## UI Screens

### Main Dashboard

```
┌─────────────────────────────────────────────────────────────┐
│ Inkbird Monitor                          [S]can [Q]uit      │
├─────────────────────────┬───────────────────────────────────┤
│  ◉ Living Room          │  ○ Bedroom (disconnected)         │
│  ┌─────────────────┐    │  ┌─────────────────┐              │
│  │   CO2: 847 ppm  │    │  │   CO2: --- ppm  │              │
│  │  Temp: 23.4°C   │    │  │  Temp: ---      │              │
│  │   Hum: 52%      │    │  │   Hum: ---      │              │
│  │  Pres: 1013 hPa │    │  │                 │              │
│  └─────────────────┘    │  └─────────────────┘              │
│  [H]istory [C]onfigure  │                                   │
├─────────────────────────┴───────────────────────────────────┤
│  ▁▂▃▄▅▆▇█▇▆▅▄▃▂▁ CO2 trend (1h)                            │
└─────────────────────────────────────────────────────────────┘
```

**Key bindings:**
- `S` - Scan for new sensors
- `H` - History screen (charts)
- `C` - Settings screen for selected sensor
- `1-4` - Select sensor tile
- `Q` - Quit

**CO2 color coding:**
- Green: < 800 ppm (good)
- Yellow: 800-1000 ppm (moderate)
- Red: > 1000 ppm (poor)

### History Screen

```
┌─────────────────────────────────────────────────────────────┐
│ History: Living Room                     [B]ack [D]ownload  │
├─────────────────────────────────────────────────────────────┤
│  Time Range: [1h] [6h] [24h] [7d] [All]                     │
├─────────────────────────────────────────────────────────────┤
│  CO2 (ppm)                                                  │
│  1200┤                                                      │
│  1000┤      ╭──╮    ╭─╮                                     │
│   800┤  ╭───╯  ╰────╯ ╰───╮                                 │
│   600┤──╯                  ╰──────────────                  │
│   400┼────────────────────────────────────                  │
│      └──────────────────────────────────────────────────    │
│       00:00    06:00    12:00    18:00    now               │
├─────────────────────────────────────────────────────────────┤
│  Metric: [CO2] [Temp] [Humidity] [Pressure]                 │
└─────────────────────────────────────────────────────────────┘
```

**Features:**
- Time range selector (1h, 6h, 24h, 7d, All)
- Metric tabs (CO2, Temp, Humidity, Pressure)
- Download button triggers BLE history download
- Auto-refresh as new readings arrive

### Settings Screen

```
┌─────────────────────────────────────────────────────────────┐
│ Settings: Living Room                    [B]ack [S]ave      │
├─────────────────────────────────────────────────────────────┤
│  ┌─ CO2 Thresholds ─────────────────────────────────────┐   │
│  │  Mode: (•) Normal  ( ) Plant/Custom                  │   │
│  │  Low threshold:   [ 420 ] ppm                        │   │
│  │  High threshold:  [2000 ] ppm                        │   │
│  │  [ ] Reset to defaults                               │   │
│  └──────────────────────────────────────────────────────┘   │
│  ┌─ Alarm ──────────────────────────────────────────────┐   │
│  │  [x] Enabled                                         │   │
│  │  Trigger at:      [1000 ] ppm                        │   │
│  └──────────────────────────────────────────────────────┘   │
│  ┌─ Calibration Offsets ────────────────────────────────┐   │
│  │  CO2:    [  0 ] ppm    Temperature: [ 0.0 ] °C       │   │
│  │  Humidity: [  0 ] %    Unit: (•) Celsius ( ) Fahr.   │   │
│  └──────────────────────────────────────────────────────┘   │
│  ┌─ Advanced ───────────────────────────────────────────┐   │
│  │  [Calibrate CO2 to 400ppm]  [Reset CO2 Sensor]       │   │
│  │  Auto-calibration: [x] Enabled                       │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

**Workflow:**
1. Screen opens → fetches current settings via BLE
2. User edits values in form widgets
3. Save button → sends commands to sensor
4. Calibration buttons show confirmation dialog first

## Async Flow & Connection Management

### SensorManager Class

```python
class SensorManager:
    """Manages BLE connections for multiple sensors"""

    async def poll_loop(self):
        """Round-robin polling, one sensor at a time"""
        while self.running:
            for sensor in self.sensors:
                if not sensor.enabled:
                    continue
                try:
                    reading = await self.read_sensor(sensor.mac)
                    self.storage.save_reading(sensor.mac, reading)
                    self.on_reading(sensor.mac, reading)  # Update UI
                except BleakError:
                    sensor.failures += 1
                    if sensor.failures >= 3:
                        sensor.skip_cycles = 5  # Back off
            await asyncio.sleep(self.poll_interval)

    async def read_sensor(self, mac) -> SensorReading:
        """Connect, pair, request data, disconnect"""
        async with BleakClient(mac) as client:
            await client.start_notify(NOTIFY_UUID, self._on_notify)
            await client.write_gatt_char(WRITE_UUID, build_pairing_cmd())
            await self._wait_for_pairing()
            await client.write_gatt_char(WRITE_UUID, build_realtime_cmd())
            return await self._wait_for_reading()
```

### UI Updates

- `SensorManager` posts `ReadingReceived` messages via Textual's message system
- Dashboard widgets subscribe and refresh on receive
- Decouples BLE operations from UI rendering

### Graceful Shutdown

- `Ctrl+C` or `Q` sets `running = False`
- Waits for current BLE operation to complete
- Closes SQLite connection cleanly

## Testing Strategy

### Project Structure (with tests)

```
inkbird_tui/
├── ...
└── tests/
    ├── __init__.py
    ├── test_protocol.py    # Packet building/parsing (no hardware needed)
    ├── test_storage.py     # SQLite operations (in-memory DB)
    ├── test_models.py      # Data class validation
    └── conftest.py         # Pytest fixtures
```

### Test Categories

**1. Protocol Tests (`test_protocol.py`)** - Most critical, no hardware needed:
```python
def test_build_pairing_command():
    cmd = build_pairing_cmd()
    assert cmd == bytes([0x55, 0xAA, 0x08, 0x06, 0x01, 0x0E])

def test_parse_realtime_response():
    # Real packet from C implementation logs
    data = bytes([0x55, 0xAA, 0x01, 0x0D, 0x00, 0x00, 0xEA, 0x02, 0x1C, 0x03, 0x4B, 0x03, 0xF5, ...])
    reading = parse_realtime_response(data)
    assert reading.co2_ppm == 843
    assert reading.temperature == 234  # 23.4°C
    assert reading.humidity == 540     # 54.0%

def test_parse_history_record():
    # 10-byte history record
    data = bytes([0x03, 0x4B, 0x00, 0x00, 0xEA, 0x02, 0x1C, 0x03, 0xF5, 0x0A])
    record = parse_history_record(data)
    assert record.co2_ppm == 843
    assert record.interval_mins == 10

def test_checksum_calculation():
    packet = bytes([0x55, 0xAA, 0x09, 0x06, 0x01])
    assert calc_checksum(packet) == 0x0F
```

**2. Storage Tests (`test_storage.py`)** - Use in-memory SQLite:
```python
@pytest.fixture
def storage():
    return Storage(":memory:")  # In-memory DB for tests

def test_save_and_retrieve_reading(storage):
    reading = SensorReading(co2_ppm=800, temperature=234, ...)
    storage.save_reading("AA:BB:CC:DD:EE:FF", reading)
    readings = storage.get_readings("AA:BB:CC:DD:EE:FF", since=0)
    assert len(readings) == 1
    assert readings[0].co2_ppm == 800

def test_auto_cleanup_old_readings(storage):
    # Insert readings older than 24h
    # Verify they get cleaned up
```

**3. Model Tests (`test_models.py`)** - Validation and edge cases:
```python
def test_sensor_reading_valid_range():
    reading = SensorReading(co2_ppm=50000, ...)  # Invalid
    assert not reading.is_valid()

def test_temperature_sign_handling():
    # Negative temperature encoding
    reading = SensorReading(temperature=-150)  # -15.0°C
    assert reading.temperature_celsius == -15.0
```

### Running Tests

```bash
# Run all tests
pytest tests/

# Run with coverage
pytest tests/ --cov=inkbird_tui --cov-report=term-missing

# Run specific test file
pytest tests/test_protocol.py -v
```

### Test Data

Use actual packet captures from C implementation logs for realistic test cases. The protocol documentation provides example packets that serve as ground truth.

## Dependencies

```
textual>=0.47.0
bleak>=0.21.0
plotext>=5.2.0

# Dev dependencies
pytest>=7.0.0
pytest-cov>=4.0.0
pytest-asyncio>=0.21.0  # For async BLE tests
```

## Implementation Order

1. `ble/protocol.py` - Packet encoding/decoding (testable standalone)
2. `data/models.py` - Data classes
3. `data/storage.py` - SQLite layer
4. `ble/client.py` - BLE connection manager
5. `ui/widgets/` - Reusable UI components
6. `ui/screens/main.py` - Dashboard
7. `ui/screens/history.py` - Charts
8. `ui/screens/settings.py` - Configuration
9. `ui/app.py` - Main app assembly
10. `main.py` - Entry point
