#!/usr/bin/env python3
"""
Demo script showing inkbird_client.py capabilities with synthetic data.
Use this to test charting without a real sensor.
"""

from datetime import datetime, timedelta
import random

from inkbird_client import SensorReading, plot_history


def generate_demo_data(hours: int = 24, interval_mins: int = 10) -> list[SensorReading]:
    """
    Generate synthetic sensor data for demonstration.

    Args:
        hours: Number of hours of data to generate
        interval_mins: Recording interval in minutes

    Returns:
        List of synthetic sensor readings
    """
    readings = []
    num_readings = (hours * 60) // interval_mins

    # Starting values with some variability
    base_co2 = 450
    base_temp = 21.0
    base_humidity = 45.0
    base_pressure = 1013

    current_time = datetime.now() - timedelta(hours=hours)

    for i in range(num_readings):
        # Simulate daily patterns
        hour_of_day = current_time.hour + (current_time.minute / 60.0)

        # CO2: Lower at night (400-500), higher during day (600-900), peaks afternoon
        if 6 <= hour_of_day < 22:
            # Daytime
            co2_factor = 1.0 + 0.6 * abs((hour_of_day - 14) / 8.0)  # Peak at 2 PM
            co2 = int(base_co2 * co2_factor + random.uniform(-50, 50))
        else:
            # Nighttime
            co2 = int(base_co2 * 0.9 + random.uniform(-30, 30))

        # Temperature: Cooler at night, warmer during day
        if 6 <= hour_of_day < 22:
            temp_factor = 1.0 + 0.15 * (1 - abs((hour_of_day - 14) / 8.0))
            temperature = base_temp * temp_factor + random.uniform(-0.5, 0.5)
        else:
            temperature = base_temp * 0.95 + random.uniform(-0.3, 0.3)

        # Humidity: Inverse of temperature (higher when cooler)
        humidity_factor = 1.2 - ((temperature - 20.0) / 10.0) * 0.3
        humidity = base_humidity * humidity_factor + random.uniform(-2, 2)

        # Pressure: Slow random walk
        base_pressure += random.uniform(-1, 1)
        base_pressure = max(1000, min(1030, base_pressure))  # Keep in realistic range

        reading = SensorReading(
            timestamp=current_time,
            co2_ppm=max(400, min(5000, co2)),
            temperature=round(temperature, 1),
            humidity=round(max(0, min(100, humidity)), 1),
            pressure=int(base_pressure),
        )
        readings.append(reading)

        current_time += timedelta(minutes=interval_mins)

    return readings


def main():
    """Generate demo data and display chart."""
    print("=" * 60)
    print("INKBIRD CLIENT DEMO - Synthetic Data")
    print("=" * 60)

    # Generate 24 hours of data at 10-minute intervals
    print("\nGenerating 24 hours of synthetic sensor data...")
    readings = generate_demo_data(hours=24, interval_mins=10)

    print(f"✓ Generated {len(readings)} readings")
    print(f"  Time range: {readings[0].timestamp} to {readings[-1].timestamp}")
    print(f"  Recording interval: 10 minutes")

    # Display statistics
    co2_values = [r.co2_ppm for r in readings]
    temp_values = [r.temperature for r in readings]
    humidity_values = [r.humidity for r in readings]

    print("\n" + "=" * 60)
    print("STATISTICS")
    print("=" * 60)
    print(
        f"CO₂:         min={min(co2_values):4d} ppm, max={max(co2_values):4d} ppm, avg={sum(co2_values) / len(co2_values):6.1f} ppm"
    )
    print(
        f"Temperature: min={min(temp_values):5.1f}°C,  max={max(temp_values):5.1f}°C,  avg={sum(temp_values) / len(temp_values):6.1f}°C"
    )
    print(
        f"Humidity:    min={min(humidity_values):5.1f}%,   max={max(humidity_values):5.1f}%,   avg={sum(humidity_values) / len(humidity_values):6.1f}%"
    )

    # Show sample readings
    print("\n" + "=" * 60)
    print("SAMPLE READINGS")
    print("=" * 60)
    print("First 3 readings:")
    for reading in readings[:3]:
        print(f"  {reading}")

    print(f"\n... {len(readings) - 6} more readings ...")

    print("\nLast 3 readings:")
    for reading in readings[-3:]:
        print(f"  {reading}")

    # Display chart
    print("\nDisplaying chart...")
    plot_history(readings, title="Inkbird IAM-T1 Demo - Synthetic Data (24h)")


if __name__ == "__main__":
    main()
