#!/usr/bin/env python3
"""
Inkbird IAM-T1 CO2 Sensor BLE Client

Downloads historical data from Inkbird IAM-T1 sensor via BLE and displays charts.
Based on reverse-engineered protocol from INKBIRD_IAM_T1_PROTOCOL.md.
"""

import argparse
import asyncio
import struct
from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import Optional

import matplotlib.pyplot as plt
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from tqdm import tqdm

# BLE UUIDs (from protocol documentation)
SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"  # Receive data
WRITE_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"  # Send commands (actual UUID: FFE1, not FFE9)

# Protocol commands
CMD_PAIR = bytes.fromhex("55AA0806010E")
CMD_REALTIME = bytes.fromhex("55AA0906010F")
CMD_HISTORY_START = bytes.fromhex("55AA0706000C")
CMD_HISTORY_STOP = bytes.fromhex("55AA0706010D")

# Protocol markers
HISTORY_END_MARKER = bytes.fromhex("6666")


@dataclass
class SensorReading:
    """Represents a single sensor reading with timestamp."""

    timestamp: datetime
    co2_ppm: int
    temperature: float  # °C
    humidity: float  # %
    pressure: int  # hPa

    def __str__(self) -> str:
        return (
            f"{self.timestamp.strftime('%Y-%m-%d %H:%M:%S')}: "
            f"CO2={self.co2_ppm}ppm, T={self.temperature:.1f}°C, "
            f"H={self.humidity:.1f}%, P={self.pressure}hPa"
        )


class InkbirdClient:
    """BLE client for Inkbird IAM-T1 CO2 sensor."""

    def __init__(self, device: BLEDevice):
        self.device = device
        self.client: Optional[BleakClient] = None

        # History download state
        self._history_records: list[bytes] = []
        self._history_expected_count: Optional[int] = None
        self._history_complete = False
        self._history_progress: Optional[tqdm] = None

    @staticmethod
    async def scan(
        timeout: float = 15.0, accept_any_ffe0: bool = False
    ) -> list[BLEDevice]:
        """
        Scan for Inkbird IAM-T1 devices.

        Args:
            timeout: Scan duration in seconds
            accept_any_ffe0: If True, accept ANY device with FFE0 service (for debugging)

        Returns:
            List of discovered Inkbird devices
        """
        print(f"Scanning for Inkbird devices ({timeout}s)...")

        # First try scanning for devices with our service UUID
        try:
            devices = await BleakScanner.discover(
                timeout=timeout, service_uuids=[SERVICE_UUID]
            )
        except Exception as e:
            print(f"  Warning: Service UUID scan failed: {e}")
            print("  Falling back to full scan...")
            devices = await BleakScanner.discover(timeout=timeout)

        inkbird_devices = []
        for device in devices:
            name = device.name or ""

            # Accept if name matches OR if accept_any_ffe0 is True
            if (
                "ink" in name.lower()
                or "iam" in name.lower()
                or (accept_any_ffe0 and device.name)
            ):
                print(f"  Found: {device.address} - {device.name}")
                inkbird_devices.append(device)

        return inkbird_devices

    async def connect(self) -> bool:
        """
        Connect to the sensor.

        Returns:
            True if connected successfully
        """
        print(f"Connecting to {self.device.address} ({self.device.name})...")

        try:
            self.client = BleakClient(self.device)
            await self.client.connect()

            if self.client.is_connected:
                print("  ✓ Connected")
                return True
            else:
                print("  ✗ Connection failed")
                return False

        except Exception as e:
            print(f"  ✗ Connection error: {e}")
            return False

    async def disconnect(self):
        """Disconnect from the sensor."""
        if self.client and self.client.is_connected:
            await self.client.disconnect()
            print("  Disconnected")

    async def pair(self) -> bool:
        """
        Pair with the sensor.

        Returns:
            True if pairing successful
        """
        if not self.client or not self.client.is_connected:
            print("  ✗ Not connected")
            return False

        print("Pairing with sensor...")

        # Simple pairing - just send the pair command
        # The sensor may or may not require this depending on state
        try:
            await self.client.write_gatt_char(WRITE_UUID, CMD_PAIR)
            await asyncio.sleep(0.5)
            print("  ✓ Pair command sent")
            return True
        except Exception as e:
            print(f"  ✗ Pairing error: {e}")
            return False

    def _parse_history_record(
        self, data: bytes
    ) -> Optional[tuple[int, float, float, int, int]]:
        """
        Parse a 10-byte history record.

        Format (from protocol doc):
        - Bytes 0-1: CO2 (big-endian)
        - Byte 2 bits[0-3]: Unit flag (0=°C, 1=°F)
        - Byte 2 bits[4-7]: Temperature sign (0=positive, 1=negative)
        - Bytes 3-4: Temperature * 10 (big-endian)
        - Bytes 5-6: Humidity * 10 (big-endian)
        - Bytes 7-8: Pressure (big-endian)
        - Byte 9: Recording interval in minutes

        Returns:
            Tuple of (co2, temp_c, humidity, pressure, interval_mins) or None if invalid
        """
        if len(data) < 10:
            return None

        # Skip empty records (flash memory may contain 0xFF)
        if data == b"\xff" * 10:
            return None

        try:
            # CO2: bytes 0-1 (big-endian)
            co2 = struct.unpack(">H", data[0:2])[0]

            # Unit & sign flags: byte 2
            unit_flag = data[2] & 0x0F
            temp_sign = (data[2] & 0xF0) >> 4

            # Temperature: bytes 3-4 (big-endian) * 0.1
            temp_raw = struct.unpack(">H", data[3:5])[0]
            temperature = temp_raw / 10.0
            if temp_sign != 0:
                temperature = -temperature

            # Humidity: bytes 5-6 (big-endian) * 0.1
            humidity = struct.unpack(">H", data[5:7])[0] / 10.0

            # Pressure: bytes 7-8 (big-endian)
            pressure = struct.unpack(">H", data[7:9])[0]

            # Interval: byte 9
            interval_mins = data[9]

            return (co2, temperature, humidity, pressure, interval_mins)

        except Exception as e:
            print(f"  Warning: Failed to parse record: {e}")
            return None

    def _history_notification_handler(self, sender, data: bytearray):
        """Handle history download notifications."""
        raw_data = bytes(data)

        # Check for end marker
        if raw_data == HISTORY_END_MARKER or HISTORY_END_MARKER in raw_data:
            self._history_complete = True
            if self._history_progress:
                self._history_progress.close()
            return

        # First response: 2-byte record count (big-endian)
        if self._history_expected_count is None:
            if len(raw_data) >= 2:
                self._history_expected_count = struct.unpack(">H", raw_data[:2])[0]

                # Initialize progress bar
                self._history_progress = tqdm(
                    total=self._history_expected_count,
                    desc="Downloading history",
                    unit="rec",
                    bar_format="{l_bar}{bar}| {n_fmt}/{total_fmt} [{elapsed}<{remaining}, {rate_fmt}]",
                )

                # Process any remaining data after count
                if len(raw_data) > 2:
                    self._process_history_data(raw_data[2:])
            return

        # Process as history record data
        self._process_history_data(raw_data)

    def _process_history_data(self, data: bytes):
        """Process history data bytes (10-byte records)."""
        # Skip end marker
        if HISTORY_END_MARKER in data:
            return

        # Process 10-byte records
        pos = 0
        while pos + 10 <= len(data):
            record = data[pos : pos + 10]
            self._history_records.append(record)

            if self._history_progress:
                self._history_progress.update(1)

            pos += 10

    async def download_history(self) -> list[SensorReading]:
        """
        Download historical data from sensor.

        Returns:
            List of sensor readings in chronological order (oldest first)
        """
        if not self.client or not self.client.is_connected:
            print("  ✗ Not connected")
            return []

        # Reset state
        self._history_records = []
        self._history_expected_count = None
        self._history_complete = False
        self._history_progress = None

        print("\nStarting history download...")

        try:
            # Start notifications
            await self.client.start_notify(
                NOTIFY_UUID, self._history_notification_handler
            )
            await asyncio.sleep(0.5)

            # Send history start command
            await self.client.write_gatt_char(WRITE_UUID, CMD_HISTORY_START)

            # Wait for download to complete (timeout: 2 minutes)
            timeout = 120
            for _ in range(timeout * 10):  # Check every 100ms
                if self._history_complete:
                    break
                await asyncio.sleep(0.1)

            # Stop notifications
            await self.client.stop_notify(NOTIFY_UUID)

            # Close progress bar if still open
            if self._history_progress:
                self._history_progress.close()

            # Parse records
            readings = []
            current_time = datetime.now()

            # Determine interval from first valid record
            interval_mins = 10  # Default fallback
            for record_data in self._history_records:
                parsed = self._parse_history_record(record_data)
                if parsed:
                    interval_mins = parsed[4] or 10
                    break

            # Parse all records and assign timestamps
            # Records are in chronological order (oldest first)
            # Work backwards from current time
            num_records = len(self._history_records)

            for i, record_data in enumerate(self._history_records):
                parsed = self._parse_history_record(record_data)
                if not parsed:
                    continue

                co2, temp, humidity, pressure, _ = parsed

                # Calculate timestamp: current time - (records_remaining * interval)
                records_from_end = num_records - i - 1
                timestamp = current_time - timedelta(
                    minutes=records_from_end * interval_mins
                )

                reading = SensorReading(
                    timestamp=timestamp,
                    co2_ppm=co2,
                    temperature=temp,
                    humidity=humidity,
                    pressure=pressure,
                )
                readings.append(reading)

            print(f"\n✓ Downloaded {len(readings)} readings")
            if readings:
                print(
                    f"  Time range: {readings[0].timestamp} to {readings[-1].timestamp}"
                )
                print(f"  Recording interval: {interval_mins} minutes")

            return readings

        except Exception as e:
            print(f"\n✗ History download error: {e}")
            if self._history_progress:
                self._history_progress.close()
            return []


def plot_history(
    readings: list[SensorReading], title: str = "Inkbird IAM-T1 Sensor Data"
):
    """
    Plot sensor readings on a single chart with multiple y-axes.

    Args:
        readings: List of sensor readings
        title: Chart title
    """
    if not readings:
        print("No data to plot")
        return

    # Extract data
    timestamps = [r.timestamp for r in readings]
    co2_values = [r.co2_ppm for r in readings]
    temp_values = [r.temperature for r in readings]
    humidity_values = [r.humidity for r in readings]

    # Create figure and primary axis
    fig, ax1 = plt.subplots(figsize=(14, 8))

    # Plot CO2 on primary y-axis (left)
    color_co2 = "tab:blue"
    ax1.set_xlabel("Time", fontsize=12)
    ax1.set_ylabel("CO₂ (ppm)", color=color_co2, fontsize=12)
    line1 = ax1.plot(timestamps, co2_values, color=color_co2, linewidth=2, label="CO₂")
    ax1.tick_params(axis="y", labelcolor=color_co2)
    ax1.grid(True, alpha=0.3)

    # Create secondary y-axis (right) for temperature and humidity
    ax2 = ax1.twinx()
    ax2.set_ylabel("Temperature (°C) / Humidity (%)", fontsize=12)

    # Plot temperature
    color_temp = "tab:red"
    line2 = ax2.plot(
        timestamps,
        temp_values,
        color=color_temp,
        linewidth=2,
        label="Temperature",
        linestyle="--",
    )

    # Plot humidity
    color_humidity = "tab:green"
    line3 = ax2.plot(
        timestamps,
        humidity_values,
        color=color_humidity,
        linewidth=2,
        label="Humidity",
        linestyle=":",
    )

    # Combine legends
    lines = line1 + line2 + line3
    labels = [line.get_label() for line in lines]
    ax1.legend(lines, labels, loc="upper left", fontsize=10)

    # Format x-axis based on time range
    duration = timestamps[-1] - timestamps[0]
    if duration.days > 7:
        # More than a week: show dates
        fig.autofmt_xdate()
    elif duration.days > 1:
        # Multiple days: show day and time
        import matplotlib.dates as mdates

        ax1.xaxis.set_major_formatter(mdates.DateFormatter("%m/%d %H:%M"))
        fig.autofmt_xdate()
    else:
        # Single day: show time only
        import matplotlib.dates as mdates

        ax1.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        fig.autofmt_xdate()

    plt.title(title, fontsize=14, fontweight="bold")
    plt.tight_layout()
    plt.show()


async def main():
    """Main entry point for CLI."""
    parser = argparse.ArgumentParser(
        description="Inkbird IAM-T1 CO2 Sensor BLE Client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --scan              # Scan for devices
  %(prog)s                     # Download history and show chart
  %(prog)s --no-chart          # Download history without chart
        """,
    )
    parser.add_argument(
        "--scan", action="store_true", help="Scan for Inkbird devices and exit"
    )
    parser.add_argument(
        "--no-chart", action="store_true", help="Skip displaying chart after download"
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=15.0,
        help="Scan timeout in seconds (default: 15)",
    )

    args = parser.parse_args()

    # Scan for devices
    devices = await InkbirdClient.scan(timeout=args.timeout)

    if not devices:
        print("\n✗ No Inkbird devices found")
        print("  Make sure the sensor is powered on and in range")
        return 1

    if args.scan:
        # Scan-only mode
        print(f"\n✓ Found {len(devices)} device(s)")
        return 0

    # Use the first device found
    device = devices[0]
    print(f"\nUsing device: {device.address} - {device.name}")

    # Create client
    client = InkbirdClient(device)

    try:
        # Connect
        if not await client.connect():
            return 1

        # Optional pairing (may not be required)
        await client.pair()

        # Download history
        readings = await client.download_history()

        if not readings:
            print("\n✗ No readings downloaded")
            return 1

        # Display summary statistics
        print("\n" + "=" * 60)
        print("STATISTICS")
        print("=" * 60)
        co2_values = [r.co2_ppm for r in readings]
        temp_values = [r.temperature for r in readings]
        humidity_values = [r.humidity for r in readings]

        print(
            f"CO₂:         min={min(co2_values):4d} ppm, max={max(co2_values):4d} ppm, avg={sum(co2_values) / len(co2_values):6.1f} ppm"
        )
        print(
            f"Temperature: min={min(temp_values):5.1f}°C,  max={max(temp_values):5.1f}°C,  avg={sum(temp_values) / len(temp_values):6.1f}°C"
        )
        print(
            f"Humidity:    min={min(humidity_values):5.1f}%,   max={max(humidity_values):5.1f}%,   avg={sum(humidity_values) / len(humidity_values):6.1f}%"
        )

        # Show first and last readings
        print("\n" + "=" * 60)
        print("SAMPLE READINGS")
        print("=" * 60)
        print("First reading:")
        print(f"  {readings[0]}")
        print("\nLast reading:")
        print(f"  {readings[-1]}")

        # Plot chart unless --no-chart
        if not args.no_chart:
            print("\nDisplaying chart...")
            plot_history(readings)

        return 0

    except KeyboardInterrupt:
        print("\n\n✗ Interrupted by user")
        return 130

    finally:
        # Cleanup
        await client.disconnect()


if __name__ == "__main__":
    import sys

    try:
        exit_code = asyncio.run(main())
        sys.exit(exit_code)
    except KeyboardInterrupt:
        print("\n\n✗ Interrupted by user")
        sys.exit(130)
