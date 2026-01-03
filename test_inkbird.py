#!/usr/bin/env python3
"""
Test script to verify Inkbird IAM-T1 sensor notifications work with bleak.
This helps determine if the issue is with our ESP32 code or the sensor itself.
"""

import asyncio
import sys
from bleak import BleakClient, BleakScanner

# Inkbird IAM-T1 sensor details
SENSOR_MAC = "62:00:A1:35:94:2B"  # Note: reversed byte order
SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"


# IAM-T1 data packet parsing
def parse_iam_t1_data(data: bytes):
    """Parse IAM-T1 notification data."""
    print(f"Raw data ({len(data)} bytes): {data.hex()}")

    # Check for data packet (16 bytes, AA 01 prefix)
    if len(data) >= 16:
        # Try offset 0
        if data[0] == 0xAA and data[1] == 0x01:
            print("Found AA 01 at offset 0")
            parse_at_offset(data, 0)
        # Try offset 1 (Python library format)
        elif len(data) >= 17 and data[1] == 0xAA and data[2] == 0x01:
            print("Found AA 01 at offset 1")
            parse_at_offset(data, 1)
        else:
            print(f"Unknown format: first bytes = {data[:4].hex()}")

    # Check for state packet (12 bytes, AA 05 prefix)
    elif len(data) >= 12:
        if data[0] == 0xAA and data[1] == 0x05:
            unit = data[10] & 0x0F
            print(f"State packet: unit = {'Fahrenheit' if unit else 'Celsius'}")
        elif data[1] == 0xAA and data[2] == 0x05:
            unit = data[11] & 0x0F
            print(
                f"State packet (offset 1): unit = {'Fahrenheit' if unit else 'Celsius'}"
            )


def parse_at_offset(data: bytes, offset: int):
    """Parse sensor data at given offset."""
    d = data[offset:]
    sign = d[4] & 0x0F
    temp_raw = (d[5] << 8) | d[6]
    temp = -temp_raw if sign else temp_raw
    humidity = (d[7] << 8) | d[8]
    co2 = (d[9] << 8) | d[10]
    pressure = (d[11] << 8) | d[12]

    print(f"=== Sensor Data ===")
    print(f"  Temperature: {temp / 10.0:.1f} °C")
    print(f"  Humidity: {humidity / 10.0:.1f} %")
    print(f"  CO2: {co2} ppm")
    print(f"  Pressure: {pressure} hPa")


def notification_handler(sender, data):
    """Handle notification data."""
    print(f"\n--- Notification received from {sender} ---")
    parse_iam_t1_data(bytes(data))


async def main():
    print("Scanning for Inkbird devices (30 seconds)...")

    # Scan for devices - longer timeout
    devices = await BleakScanner.discover(timeout=30.0)

    inkbird_device = None
    for d in devices:
        name = d.name or ""
        addr = d.address or ""
        # Check by name or by known MAC patterns
        if "ink" in name.lower() or "iam" in name.lower():
            print(f"Found by name: {d.address} - {d.name} (RSSI: {d.rssi})")
            inkbird_device = d
        # Also check if it matches our known sensor (macOS uses UUIDs, not MACs)
        # So we need to scan and look for service UUID

    # If not found by name, look for devices advertising FFE0 service
    if not inkbird_device:
        print("\nScanning for FFE0 service...")
        scanner = BleakScanner(service_uuids=[SERVICE_UUID])
        devices2 = await scanner.discover(timeout=30.0)
        for d in devices2:
            print(f"Found with FFE0: {d.address} - {d.name}")
            inkbird_device = d
            break

    if not inkbird_device:
        print("No Inkbird device found!")
        print("\nAll devices found:")
        for d in devices:
            print(f"  {d.address} - {d.name}")
        return

    print(f"\nConnecting to {inkbird_device.address} ({inkbird_device.name})...")

    async with BleakClient(inkbird_device) as client:
        print(f"Connected: {client.is_connected}")

        # List services
        print("\nServices:")
        for service in client.services:
            print(f"  {service.uuid}: {service.description}")
            for char in service.characteristics:
                props = ", ".join(char.properties)
                print(f"    {char.uuid}: {props}")

        # Find our notification characteristic
        print(f"\nStarting notifications on {NOTIFY_UUID}...")

        try:
            await client.start_notify(NOTIFY_UUID, notification_handler)
            print("Notifications started. Waiting for data (Ctrl+C to stop)...")
            print("Note: IAM-T1 may take 1-2 minutes to send first notification")

            # Wait for notifications
            for i in range(180):  # 3 minutes
                await asyncio.sleep(1)
                if i % 30 == 0:
                    print(f"Waiting... ({i}s)")

        except Exception as e:
            print(f"Error: {e}")
        finally:
            try:
                await client.stop_notify(NOTIFY_UUID)
            except:
                pass


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped by user")
