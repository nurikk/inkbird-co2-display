Import("env")

def monitor_callback(*args, **kwargs):
    import serial
    from platformio.device.finder import SerialPortFinder

    port = env.GetProjectOption("monitor_port", None)
    baud = env.GetProjectOption("monitor_speed", 115200)

    if not port:
        port = SerialPortFinder(
            board_config=env.BoardConfig(),
            upload_protocol=env.subst("$UPLOAD_PROTOCOL"),
        ).find()

    if not port:
        print("No serial port found")
        return

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
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        if 'ser' in locals():
            ser.close()

env.AddCustomTarget(
    name="mon",
    dependencies=None,
    actions=[monitor_callback],
    title="Serial Monitor",
    description="Custom serial monitor"
)
