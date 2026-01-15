#!/usr/bin/env python3
import serial
import serial.tools.list_ports
import sys

def find_esp32_port():
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if "USB" in port.description or "UART" in port.description or "CP210" in port.description or "CH340" in port.description:
            return port.device
    if ports:
        return ports[0].device
    return None

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_esp32_port()
    if not port:
        print("No serial port found")
        sys.exit(1)

    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    print(f"Connecting to {port} at {baud} baud...")

    try:
        ser = serial.Serial(port, baud, timeout=1)
        print("Connected. Press Ctrl+C to exit.\n")
        while True:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace')
                print(line, end='')
    except serial.SerialException as e:
        print(f"Serial error: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        if 'ser' in locals():
            ser.close()

if __name__ == "__main__":
    main()
