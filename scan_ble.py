#!/usr/bin/env python3
"""
Comprehensive BLE scanner to help find Inkbird IAM-T1 sensor.
"""

import asyncio
import sys
from bleak import BleakScanner

# Known Inkbird patterns
INKBIRD_PATTERNS = [
    "ink",
    "iam",
    "inkbird",
    "ffe0",  # Service UUID
]


async def detailed_scan(duration: float = 30.0):
    """Perform detailed BLE scan with analysis."""

    print(f"🔍 Scanning for BLE devices ({duration}s)...")
    print("=" * 80)

    # Scan for devices
    devices = await BleakScanner.discover(timeout=duration, return_adv=True)

    if not devices:
        print("✗ No BLE devices found")
        print("\nTroubleshooting:")
        print("  - Check Bluetooth is enabled on this computer")
        print("  - Ensure you're running with proper permissions")
        print("  - Try moving closer to BLE devices")
        return

    print(f"\n✓ Found {len(devices)} BLE devices\n")

    # Categorize devices
    inkbird_devices = []
    named_devices = []
    unnamed_devices = []

    for address, (device, adv_data) in devices.items():
        name = device.name or ""

        # Check if it matches Inkbird patterns
        is_inkbird = any(pattern in name.lower() for pattern in INKBIRD_PATTERNS)

        # Check service UUIDs
        service_uuids = (
            adv_data.service_uuids if hasattr(adv_data, "service_uuids") else []
        )
        has_ffe0 = any("ffe0" in str(uuid).lower() for uuid in service_uuids)

        if is_inkbird or has_ffe0:
            inkbird_devices.append((device, adv_data, service_uuids))
        elif name:
            named_devices.append((device, adv_data, service_uuids))
        else:
            unnamed_devices.append((device, adv_data, service_uuids))

    # Display Inkbird devices first (if any)
    if inkbird_devices:
        print("🎯 INKBIRD DEVICES FOUND:")
        print("-" * 80)
        for device, adv_data, service_uuids in inkbird_devices:
            print(f"  ✓ {device.name or '(no name)'}")
            print(f"    Address: {device.address}")
            print(
                f"    RSSI: {adv_data.rssi if hasattr(adv_data, 'rssi') else 'N/A'} dBm"
            )
            if service_uuids:
                print(f"    Services:")
                for uuid in service_uuids:
                    print(f"      - {uuid}")
            print()
    else:
        print("⚠️  NO INKBIRD DEVICES FOUND")
        print("-" * 80)
        print("  Expected device name: 'Ink@IAM-T1' or similar")
        print("  Expected service UUID: 0000ffe0-0000-1000-8000-00805f9b34fb")
        print()

    # Display other named devices
    if named_devices:
        print(f"\n📱 Other Named Devices ({len(named_devices)}):")
        print("-" * 80)
        for device, adv_data, service_uuids in named_devices[:10]:  # Limit to 10
            rssi = adv_data.rssi if hasattr(adv_data, "rssi") else "N/A"
            print(f"  • {device.name[:40]:40} RSSI: {rssi}")
        if len(named_devices) > 10:
            print(f"  ... and {len(named_devices) - 10} more")

    # Display unnamed devices count
    if unnamed_devices:
        print(f"\n📡 Unnamed Devices: {len(unnamed_devices)}")

    # Troubleshooting tips
    if not inkbird_devices:
        print("\n" + "=" * 80)
        print("TROUBLESHOOTING TIPS:")
        print("=" * 80)
        print("1. Power cycle the Inkbird IAM-T1 sensor:")
        print("   - Remove battery")
        print("   - Wait 10 seconds")
        print("   - Reinsert battery")
        print("   - Check that display turns on")
        print()
        print("2. Close any apps connected to the sensor:")
        print("   - Inkbird mobile app")
        print("   - Other BLE apps")
        print()
        print("3. Move closer to the sensor:")
        print("   - BLE range is typically 10-30 meters")
        print("   - Walls and obstacles reduce range")
        print()
        print("4. Check battery level:")
        print("   - Low battery reduces BLE transmission power")
        print("   - Replace with fresh CR2032 battery if needed")
        print()
        print("5. Verify sensor is in pairing mode:")
        print("   - Some sensors require button press to enable BLE")
        print("   - Check user manual for pairing procedure")
        print()
        print("6. macOS permissions (if on macOS):")
        print("   - System Settings → Privacy & Security → Bluetooth")
        print("   - Ensure Terminal/Python has permission")
        print()
        print("Try running this scan again after troubleshooting.")


def main():
    """Main entry point."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Scan for BLE devices, especially Inkbird IAM-T1 sensors"
    )
    parser.add_argument(
        "--duration",
        "-d",
        type=float,
        default=30.0,
        help="Scan duration in seconds (default: 30)",
    )

    args = parser.parse_args()

    try:
        asyncio.run(detailed_scan(args.duration))
    except KeyboardInterrupt:
        print("\n\n✗ Scan interrupted by user")
        sys.exit(130)


if __name__ == "__main__":
    main()
