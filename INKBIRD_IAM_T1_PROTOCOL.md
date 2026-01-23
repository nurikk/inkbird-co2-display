# INKBIRD IAM-T1 BLE Protocol Documentation

Reverse engineered from the INKBIRD Android application (decompiled).

The IAM-T1 is a Bluetooth Low Energy air quality monitor measuring:
- CO2 concentration (PPM)
- Temperature (Celsius/Fahrenheit)
- Humidity (%)
- Atmospheric pressure (hPa)

---

## 1. BLE Device Identification

### Advertisement Name
```
Ink@IAM-T1
```

The device advertises with the prefix `Ink@IAM-T1` in its Bluetooth device name.

---

## 2. BLE Service and Characteristic UUIDs

### Primary Service
```
UUID: 0000ffe0-0000-1000-8000-00805f9b34fb
```
The service UUID starts with `0000ffe0`.

### Characteristics

| Characteristic | UUID Prefix | Properties | Purpose |
|----------------|-------------|------------|---------|
| Write | `0000ffe9` | Write | Send commands to device |
| Notify | `0000ffe4` | Notify | Receive data/responses from device |

Full UUID format: `0000ffe9-0000-1000-8000-00805f9b34fb`

---

## 3. Packet Structure

All packets use a common structure:

```
+--------+--------+--------+--------+--------+----------+
| Header | Cmd ID | Length | Data   | ...    | Checksum |
| 2 bytes| 1 byte | 1 byte | N bytes|        | 1 byte   |
+--------+--------+--------+--------+--------+----------+
```

### Header
All packets start with magic bytes: `55 AA`

### Command ID
Single byte identifying the command type (see Command Reference).

### Length
Data length in bytes (varies by command).

### Checksum
Sum of all bytes in the packet, AND with `0xFF` (lower 8 bits only).

**Checksum calculation:**
```
checksum = (sum of all bytes including header) & 0xFF
```

---

## 4. Command Reference

### 4.1 Pairing Commands (Command ID: 0x08)

#### Pairing Request
Sent by app to initiate pairing:
```
55 AA 08 06 01 0E
```

| Byte | Value | Description |
|------|-------|-------------|
| 0-1 | 55 AA | Header |
| 2 | 08 | Command ID: Pairing |
| 3 | 06 | Length |
| 4 | 01 | Pairing request flag |
| 5 | 0E | Checksum |

#### Pairing Acknowledgment (Device Response)
Device not yet paired:
```
55 AA 08 06 00 0D
```

#### Pairing Success (Device Response)
Device confirms pairing:
```
55 AA 08 06 02 0F
```

---

### 4.2 Real-Time Data Request (Command ID: 0x09)

#### Request Real-Time Data
```
55 AA 09 06 01 0F
```

| Byte | Value | Description |
|------|-------|-------------|
| 0-1 | 55 AA | Header |
| 2 | 09 | Command ID: Data request |
| 3 | 06 | Length |
| 4 | 01 | Request flag |
| 5 | 0F | Checksum |

---

### 4.3 CO2 Settings (Command ID: 0x02)

#### Set CO2 Mode/Calibration Settings
```
55 AA 02 0B <mode> <custom> <auto> <manual_mode> <manual_cal_hi> <manual_cal_lo> <checksum>
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 | Header | 55 AA |
| 2 | 1 | Cmd ID | 02 |
| 3 | 1 | Length | 0B |
| 4 | 1 | Mode | Display mode (0-4) |
| 5 | 1 | Custom | Custom mode flag (00/01) |
| 6 | 1 | Automatic | Auto calibration flag (00/01) |
| 7 | 1 | Manual Mode | 0=off, 1=calibrating, 4=reset CO2 |
| 8-9 | 2 | Manual Cal | Manual calibration value (big-endian) |
| 10 | 1 | Checksum | Sum & 0xFF |

**Manual Mode Values:**
- `0x00` - Normal operation
- `0x01` - Manual calibration in progress
- `0x04` - Reset CO2 sensor

---

### 4.4 CO2 Threshold Settings (Command ID: 0x03)

#### Set CO2 High/Low Thresholds
```
55 AA 03 0E <high_co2> <low_co2> <high_plant> <low_plant> <reset> <checksum>
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 | Header | 55 AA |
| 2 | 1 | Cmd ID | 03 |
| 3 | 1 | Length | 0E |
| 4-5 | 2 | High CO2 | Normal mode high threshold (big-endian) |
| 6-7 | 2 | Low CO2 | Normal mode low threshold (big-endian) |
| 8-9 | 2 | High Plant CO2 | Plant mode high threshold |
| 10-11 | 2 | Low Plant CO2 | Plant mode low threshold |
| 12 | 1 | Reset | Reset thresholds flag (00/01) |
| 13 | 1 | Checksum | Sum & 0xFF |

**Default Values:**
- Normal mode: Low 420 PPM, High 2000 PPM
- Plant mode: Low 340 PPM, High 5000 PPM

---

### 4.5 CO2 Alarm Settings (Command ID: 0x04)

#### Set CO2 Alarm (IAM-T1)
```
55 AA 04 09 <enabled> <alarm_mode> <alarm_value_hi> <alarm_value_lo> <checksum>
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 | Header | 55 AA |
| 2 | 1 | Cmd ID | 04 |
| 3 | 1 | Length | 09 (IAM-T1) or 0A (IAM-T2) |
| 4 | 1 | Enabled | 00=alarm off, 01=alarm on |
| 5 | 1 | Alarm Mode | Alarm type/behavior |
| 6-7 | 2 | Alarm Value | CO2 alarm threshold (big-endian) |
| 8 | 1 | Checksum | Sum & 0xFF |

Note: IAM-T2 adds an extra byte for LED control.

---

### 4.6 Calibration Values (Command ID: 0x05)

#### Set Calibration Offsets
```
55 AA 05 0C <co2_sign> <co2_cal> <temp_sign> <temp_cal> <hum_sign> <hum_cal> <unit> <checksum>
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 | Header | 55 AA |
| 2 | 1 | Cmd ID | 05 |
| 3 | 1 | Length | 0C |
| 4 | 1 | CO2 Sign | 00=positive, 01=negative |
| 5 | 1 | CO2 Offset | CO2 calibration offset (PPM) |
| 6 | 1 | Temp Sign | 00=positive, 01=negative |
| 7 | 1 | Temp Offset | Temperature offset * 10 |
| 8 | 1 | Hum Sign | 00=positive, 01=negative |
| 9 | 1 | Hum Offset | Humidity offset * 10 |
| 10 | 1 | Unit | 00=Celsius, 01=Fahrenheit |
| 11 | 1 | Checksum | Sum & 0xFF |

---

### 4.7 History Data Commands (Command ID: 0x07)

#### Request History Download
```
55 AA 07 06 00 0C
```

| Byte | Value | Description |
|------|-------|-------------|
| 0-1 | 55 AA | Header |
| 2 | 07 | Command ID: History |
| 3 | 06 | Length |
| 4 | 00 | Start download |
| 5 | 0C | Checksum |

#### Cancel History Download
```
55 AA 07 06 01 0D
```

| Byte | Value | Description |
|------|-------|-------------|
| 0-1 | 55 AA | Header |
| 2 | 07 | Command ID: History |
| 3 | 06 | Length |
| 4 | 01 | Cancel download |
| 5 | 0D | Checksum |

---

## 5. Response Data Formats

### 5.1 Real-Time Data Response

Received via notify characteristic. Minimum length: 22 hex characters (11 bytes).

**Data format (hex string, big-endian):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 char | Temp Unit | 0=Celsius, 1=Fahrenheit |
| 1 | 1 char | Temp Sign | 0=positive, 1=negative |
| 2-5 | 4 chars | Temperature | Value / 10.0 |
| 6-9 | 4 chars | Humidity | Value / 10 (%) |
| 10-13 | 4 chars | CO2 | PPM value |
| 14-17 | 4 chars | Pressure | hPa value |
| 18-19 | 2 chars | Battery/Charge | Lower 4 bits=level, bit 7=charging |
| 20-21 | 2 chars | Mark | Status/mark byte |

**Parsing example:**
```
Response: "0100FA023C02BC04B0150A"

Temp Unit: 0 = Celsius
Temp Sign: 1 = negative
Temperature: 0x00FA = 250 / 10 = -25.0°C
Humidity: 0x023C = 572 / 10 = 57%
CO2: 0x02BC = 700 PPM
Pressure: 0x04B0 = 1200 hPa
Battery: 0x15 = level 5 (0x15 & 0x0F), not charging
Mark: 0x0A
```

---

### 5.2 CO2 Settings Response

Response length: minimum 12 hex characters (6 bytes).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 chars | Mode | Display mode |
| 2-3 | 2 chars | Custom | Custom mode (01=enabled) |
| 4-5 | 2 chars | Automatic | Auto calibration (01=enabled) |
| 6-7 | 2 chars | Manual Mode | Manual mode state |
| 8-11 | 4 chars | Manual Cal | Manual calibration value |

---

### 5.3 CO2 Thresholds Response

Response length: minimum 18 hex characters (9 bytes).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-3 | 4 chars | High CO2 | Normal mode high threshold |
| 4-7 | 4 chars | Low CO2 | Normal mode low threshold |
| 8-11 | 4 chars | High Plant CO2 | Plant mode high |
| 12-15 | 4 chars | Low Plant CO2 | Plant mode low |

---

### 5.4 CO2 Alarm Response

Response length: minimum 8 hex characters (4 bytes).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 chars | CO2 Off | 00=alarm enabled, other=disabled |
| 2-3 | 2 chars | Alarm Mode | Alarm behavior mode |
| 4-7 | 4 chars | (optional) | Additional data |
| 8-9 | 2 chars | LED On | (IAM-T2 only) 01=LED enabled |

---

### 5.5 Calibration Values Response

Response length: minimum 14 hex characters (7 bytes).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 chars | CO2 Sign | 00=positive, 01=negative |
| 2-3 | 2 chars | CO2 Offset | PPM offset value |
| 4-5 | 2 chars | Temp Sign | 00=positive, 01=negative |
| 6-7 | 2 chars | Temp Offset | Offset / 10.0 |
| 8-9 | 2 chars | Hum Sign | 00=positive, 01=negative |
| 10-11 | 2 chars | Hum Offset | Offset / 10 |
| 12-13 | 2 chars | Unit | 00=Celsius, 01=Fahrenheit |

---

### 5.6 Alarm Status Response

Response length: minimum 2 hex characters (1 byte).

| Bit | Field | Description |
|-----|-------|-------------|
| 0 | High CO2 Alarm | 1=active |
| 1 | Low Temp Alarm | 1=active |

---

## 6. History Download Protocol

### 6.1 Protocol Flow

1. **Connect WITHOUT pairing**: History download requires a fresh BLE connection without sending the pairing command. If you pair first, the sensor starts sending periodic real-time data notifications that interfere with the history data stream.
2. **Start download**: Send `55 AA 07 06 00 0C`
3. **Receive record count**: 2 bytes (binary big-endian) = number of records
4. **Receive data stream**: Continuous raw data packets (no packet headers)
5. **End marker**: `66 66` detected in stream
6. **Cancel if needed**: Send `55 AA 07 06 01 0D`

### 6.2 Response Format

**Initial response (record count):**

The first 2 bytes received after sending the history start command contain the record count as a 16-bit big-endian integer (raw binary, NOT ASCII hex).

Example: `0E 56` = 0x0E56 = 3670 records

> **Note**: Earlier documentation incorrectly stated this was "4 hex characters". Testing confirms it is 2 raw bytes in big-endian format.

**Data stream:**

Records are streamed continuously as raw bytes without the `55AA` header. Each record is 10 bytes.

**End marker:**
```
66 66
```
The stream ends when bytes `0x66 0x66` are detected.

### 6.3 History Record Format

Each record is 10 bytes (raw binary):

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 bytes | CO2 | CO2 value in PPM (big-endian) |
| 2 | 1 byte | Unit/Sign | Upper nibble: 0=Celsius, 1=Fahrenheit; Lower nibble: 0=positive temp, 1=negative |
| 3-4 | 2 bytes | Temperature | Value / 10.0 °C (big-endian) |
| 5-6 | 2 bytes | Humidity | Value / 10.0 % (big-endian) |
| 7-8 | 2 bytes | Pressure | hPa value (big-endian) |
| 9 | 1 byte | Time Interval | Minutes since previous record (0-59) |

### 6.4 Timestamp Reconstruction

History records don't contain absolute timestamps. Timestamps are reconstructed relative to the current time:

1. First record = current device time (rounded to minute)
2. Subsequent records use `timeInterval` field to calculate offset
3. If `timeInterval(n-1) > timeInterval(n)`: subtract `(diff * 60 * 1000)` ms
4. If they differ but current is larger: hour rollover, subtract `((prev + 60 - curr) * 60 * 1000)` ms

### 6.5 Data Validation

- Expected data length: `record_count * 10` bytes
- Valid CO2 range: 200-10000 PPM (values outside this range indicate invalid/empty records)
- Empty records may contain `0xFF` bytes (uninitialized flash memory)
- Empty response (`66 66` immediately after count of 0): No history data available

---

## 7. Connection Sequence

### 7.1 Initial Connection
1. Scan for devices with name containing `Ink@IAM-T1`
2. Connect to device
3. Discover services
4. Find service `0000ffe0-...`
5. Get write characteristic `0000ffe9-...`
6. Get notify characteristic `0000ffe4-...`
7. Enable notifications on `0000ffe4`

### 7.2 Pairing Sequence
1. Send pairing request: `55 AA 08 06 01 0E`
2. Wait for response on notify characteristic
3. If `55 AA 08 06 00 0D`: Device ready, continue pairing
4. If `55 AA 08 06 02 0F`: Pairing successful
5. Save device MAC address for future connections

### 7.3 Data Reading Sequence
1. Ensure connected and notifications enabled
2. Send data request: `55 AA 09 06 01 0F`
3. Parse response per real-time data format
4. Repeat every ~1 second for continuous monitoring

---

## 8. Data Units and Ranges

| Measurement | Range | Unit | Resolution |
|-------------|-------|------|------------|
| CO2 | 0-9999 | PPM | 1 PPM |
| Temperature | -40 to +125 | °C/°F | 0.1° |
| Humidity | 0-99 | % | 1% |
| Pressure | 300-1200 | hPa | 1 hPa |
| Battery | 0-15 | level | 1 |

### Pressure Unit Conversion
- Default: hPa
- mmHg conversion available in app

---

## 9. Error Handling

### Connection Timeout
- Pairing timeout: 30 seconds
- Reconnection delay: 500ms after disconnect

### History Download Timeout
- Fail timeout: 1000ms between packets
- Send cancel command on timeout: `55 AA 07 06 01 0D`

### Write Characteristic Not Found
- Disconnect device
- Set BLE state to 0 (disconnected)
- Trigger reconnection

---

## 10. References

**Source Files Analyzed:**
- `IadW1Model.java` - Core protocol parsing/building
- `IadW1Presenter.java` - BLE communication handler
- `IadW1SendInstructionsPresenter.java` - Command sender
- `IadW1HistoryPresenter.java` - History download protocol
- `PairingDeviceActivity.java` - Pairing sequence
- `ScanDeviceActivity.java` - Device discovery
- `IamT1Item.java` - Home screen data handling
- `IadW1Bean.java` - Data model definitions
- `DataBean.java` - History record structure

---

*Document generated from reverse engineering of INKBIRD Android application.*
*Protocol version as of app analysis date.*
