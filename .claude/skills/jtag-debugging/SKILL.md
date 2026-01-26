---
name: jtag-debugging
description: Use when debugging ESP32/ESP32-S3 with JTAG, setting breakpoints, inspecting variables, or when serial printf debugging is insufficient
allowed-tools: Bash, Read, Edit, Write
---

# JTAG Debugging for ESP32

Debug ESP32 firmware using JTAG/OpenOCD with PlatformIO. Use this when printf debugging is insufficient or you need to inspect variables, set breakpoints, or step through code.

## Prerequisites

- JTAG probe (ESP-PROG, Olimex ARM-USB-OCD-H, or built-in USB-JTAG on ESP32-S3)
- Properly connected JTAG pins
- Debug-enabled firmware (not optimized out)

## JTAG Pin Connections

### ESP32 with External Probe (e.g., ESP-PROG, Olimex)

| ESP32 Pin | JTAG Signal | Function |
|-----------|-------------|----------|
| GPIO 12 | TDI | Test Data In |
| GPIO 13 | TCK | Test Clock |
| GPIO 14 | TMS | Test Mode Select |
| GPIO 15 | TDO | Test Data Out |
| EN | nTRST | Reset (optional) |
| GND | GND | Ground |
| 3.3V | VTref | Reference voltage |

### ESP32-S3 with Built-in USB-JTAG

ESP32-S3 has built-in USB-JTAG on GPIO 19 (D-) and GPIO 20 (D+). No external probe needed.

## PlatformIO Configuration

Add to `platformio.ini`:

```ini
[env:debug]
platform = espressif32
board = esp32dev  ; or esp32-s3-devkitc-1
framework = espidf

; Debug configuration
debug_tool = esp-prog          ; or: olimex-arm-usb-ocd-h, esp-builtin (S3)
debug_init_break = tbreak app_main
debug_speed = 5000             ; JTAG clock in kHz

; Build with debug symbols
build_type = debug

; Optional: Keep optimization low for better debugging
build_flags =
    -O0
    -ggdb3
```

### Supported Debug Tools

| Tool | `debug_tool` value | Notes |
|------|-------------------|-------|
| ESP-PROG | `esp-prog` | Official Espressif debugger |
| Olimex ARM-USB-OCD-H | `olimex-arm-usb-ocd-h` | Popular third-party |
| ESP32-S3 built-in | `esp-builtin` | No external hardware needed |
| J-Link | `jlink` | Segger J-Link |

## Debugging Workflow

### 1. Build with Debug Symbols

```bash
pio run -e debug
```

### 2. Start OpenOCD Server (Terminal 1)

```bash
# PlatformIO handles this automatically, but for manual control:
~/.platformio/packages/tool-openocd-esp32/bin/openocd \
    -f interface/ftdi/esp32_devkitj_v1.cfg \
    -f target/esp32.cfg
```

### 3. Start GDB Session (Terminal 2)

```bash
# Automatic via PlatformIO (recommended)
pio debug

# Or manual GDB connection
~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gdb \
    .pio/build/debug/firmware.elf \
    -ex "target remote :3333"
```

## Common GDB Commands

### Execution Control

| Command | Shortcut | Description |
|---------|----------|-------------|
| `continue` | `c` | Resume execution |
| `next` | `n` | Step over (next line) |
| `step` | `s` | Step into function |
| `finish` | `fin` | Run until function returns |
| `until <line>` | `u` | Run until line number |

### Breakpoints

| Command | Description |
|---------|-------------|
| `break app_main` | Break at function |
| `break main.c:42` | Break at file:line |
| `break *0x400d1234` | Break at address |
| `info breakpoints` | List breakpoints |
| `delete 1` | Delete breakpoint #1 |
| `disable 1` | Disable breakpoint #1 |
| `clear main.c:42` | Clear breakpoint at location |

### Inspection

| Command | Description |
|---------|-------------|
| `print var` | Print variable value |
| `print/x var` | Print as hex |
| `print *ptr` | Dereference pointer |
| `print array[0]@10` | Print 10 array elements |
| `info locals` | Show local variables |
| `info args` | Show function arguments |
| `backtrace` | Show call stack |
| `frame 2` | Switch to stack frame #2 |

### Memory Examination

| Command | Description |
|---------|-------------|
| `x/10xw 0x3FFB0000` | Examine 10 words at addr |
| `x/s 0x3F400100` | Examine as string |
| `x/20i $pc` | Disassemble 20 instructions |

### Watchpoints (Hardware Breakpoints)

| Command | Description |
|---------|-------------|
| `watch var` | Break when var changes |
| `rwatch var` | Break when var is read |
| `awatch var` | Break on read or write |
| `info watchpoints` | List watchpoints |

## OpenOCD Commands (via GDB)

Execute OpenOCD commands through GDB's `monitor` command:

```gdb
monitor reset halt          # Reset and halt CPU
monitor reg                 # Show all registers
monitor reg pc              # Show program counter
monitor flash write_image erase firmware.bin 0x10000  # Flash firmware
monitor mdw 0x3FF44000 4    # Memory display word (4 words)
monitor mww 0x3FF44000 0x1  # Memory write word
```

## Troubleshooting

### OpenOCD Can't Connect

```bash
# Check USB permissions (Linux)
sudo usermod -a -G dialout $USER

# Check if device is detected
lsusb | grep -i ftdi

# Try lower JTAG speed
# In platformio.ini: debug_speed = 1000
```

### GDB Shows "Remote 'g' packet reply is too long"

Add to `platformio.ini`:
```ini
debug_extra_cmds =
    set remote hardware-watchpoint-limit 2
```

### Breakpoints Not Working

1. Ensure `build_type = debug` in platformio.ini
2. Check optimization level (use `-O0` for best debugging)
3. Verify JTAG connections
4. Try `monitor reset halt` before setting breakpoints

### Variables Show `<optimized out>`

Reduce optimization:
```ini
build_flags =
    -O0
    -fno-inline
```

### Flash Encryption / Secure Boot Interference

JTAG is disabled when flash encryption or secure boot is enabled. This is a security feature and cannot be bypassed.

## ESP32-S3 USB-JTAG Specific

For ESP32-S3 with built-in USB-JTAG:

```ini
[env:esp32s3-debug]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
debug_tool = esp-builtin
build_type = debug

; USB-JTAG is on separate USB port from UART
; Connect both USB ports for debugging + serial monitor
```

## Quick Reference Card

```
Start debugging:     pio debug
Set breakpoint:      b function_name  OR  b file.c:line
Run:                 c (continue)
Step over:           n (next)
Step into:           s (step)
Print variable:      p variable_name
Print hex:           p/x variable
Call stack:          bt (backtrace)
Local variables:     info locals
Reset target:        monitor reset halt
Quit:                q
```

## Example Debug Session

```bash
$ pio debug

(gdb) target remote :3333
(gdb) monitor reset halt
(gdb) break app_main
(gdb) continue
Breakpoint 1, app_main () at src/main.c:42
(gdb) info locals
sensor_value = 420
status = ESP_OK
(gdb) break read_sensor
(gdb) continue
Breakpoint 2, read_sensor () at src/sensor.c:15
(gdb) backtrace
#0  read_sensor () at src/sensor.c:15
#1  app_main () at src/main.c:48
(gdb) print sensor_config
$1 = {i2c_port = 0, address = 0x5a, timeout_ms = 1000}
(gdb) continue
```

## When NOT to Use JTAG

- Quick printf-style debugging (use `ESP_LOGI` instead)
- Timing-sensitive code (JTAG halts can break real-time behavior)
- Production firmware (JTAG should be disabled)
- Wi-Fi/Bluetooth debugging (radio timing is critical)

## See Also

- [ESP-IDF JTAG Debugging Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/jtag-debugging/)
- [PlatformIO Debugging](https://docs.platformio.org/en/latest/plus/debugging.html)
- [OpenOCD Commands](http://openocd.org/doc/html/General-Commands.html)
