# NimBLE Migration Progress

## Status: FIXED ✅

Successfully migrated from Bluedroid to NimBLE BLE stack with Multiple Connection Central functionality fully working.

## Critical Bug Fix (2025-01-17)

### Issue: Configured sensors not connecting, only auto-discovered sensors worked

**Root Cause**: MAC address byte order mismatch between configuration format and NimBLE format.

- `inkbird_config.h` stores MACs in **big-endian** (human-readable: `{0x62, 0x00, 0xA1, ...}`)
- NimBLE's `ble_addr_t.val` uses **little-endian** format (LSB first)
- `peer_add()` was copying MACs directly without byte reversal
- Auto-discovered sensors worked because their MACs were already in NimBLE format

**Fix**: Modified `inkbird_ble.c` to:
1. Reverse MAC bytes when creating peer from configured sensor (`peer_add()`)
2. Compare MACs with byte reversal in `inkbird_ble_register_discovered()`
3. Convert discovered MACs to big-endian when auto-registering

### Files Changed
- `src/ble/inkbird_ble.c`: Fixed `peer_add()` and `inkbird_ble_register_discovered()`

## Verification Results

### All Sensors Connecting: PASS ✅
- **Office** (62:00:A1:35:94:2B): Connected, receiving data
- **Leysan** (62:00:A1:3F:B2:79): Connected, receiving data
- **Bedroom** (62:00:A1:3F:B3:93): Connected, receiving data
- **Auto-discovered** (Ink@IAM-T1): Connected, receiving data

### Historical Data Download: PASS ✅
- History download connects successfully
- Records received with downsampling
- Time span covers expected range (~11 days)

### Main Screen: PASS ✅
- Display renders correctly with 4 sensor tiles
- Sensor data displays: CO2 (ppm), temperature (°C), humidity (%)
- Status messages update correctly
- Chart bars appear in tiles with data

### Detail Chart: PASS ✅
- Detail view loads history on demand
- Chart displays extended history data

## Technical Details

### MAC Byte Order Convention
```
Config format (big-endian):   {0x62, 0x00, 0xA1, 0x35, 0x94, 0x2B}
                               ^^^^                          ^^^^
                               MSB (printed first)           LSB

NimBLE format (little-endian): val[0]=0x2B, val[1]=0x94, ..., val[5]=0x62
                               ^^^^                          ^^^^
                               LSB (index 0)                 MSB (index 5)
```

### Key Code Change
```c
// peer_add() - reverse bytes when copying from config to NimBLE
for (int i = 0; i < 6; i++) {
    peer->remote_addr.val[i] = s_active_sensors[sensor_idx].mac[5 - i];
}
```

## Memory Usage
- RAM: 33.3% (109,280 / 327,680 bytes)
- Flash: 65.4% (~1,004,000 / 1,536,000 bytes)
