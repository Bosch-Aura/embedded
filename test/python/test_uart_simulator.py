#!/usr/bin/env python3
import sys
import time
import argparse
from typing import List

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError:
    print("pyserial not installed. Install with: pip install pyserial", file=sys.stderr)
    sys.exit(1)


HELP_TEXT = """
Commands (type and press Enter):
  speed <0-255>       Set motor speed (C:SET_SPEED)
  steer <50-160>      Set steering angle (C:SET_STEER) - 50=right, 105=center, 160=left
  lights <off|on|auto>  Set lights mode (not handled over UART by firmware)
  emergency           Trigger emergency brake (E:BRAKE_NOW)
  stop                Alias for speed 0
  arm                 Arm the system (M:SYS_ARM) - REQUIRED before control commands
  disarm              Disarm the system (M:SYS_DISARM)
  demo                Send a short demo sequence
  help                Show this help
  quit / exit         Leave the simulator
""".strip()


def select_port_interactive() -> str:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found. Connect the ESP32 and try again.")
        sys.exit(1)
    if len(ports) == 1:
        return ports[0].device
    print("Available serial ports:")
    for idx, p in enumerate(ports):
        print(f"  [{idx}] {p.device} - {p.description}")
    while True:
        sel = input("Select port index: ").strip()
        if sel.isdigit() and 0 <= int(sel) < len(ports):
            return ports[int(sel)].device
        print("Invalid selection. Try again.")


def open_serial(port: str, baud: int) -> serial.Serial:
    """Open serial port with appropriate settings."""
    ser = serial.Serial(
        port=port,
        baudrate=baud,
        timeout=1.0,  # Increased timeout for reading
        write_timeout=1.0,  # Timeout for writing
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        xonxoff=False,  # No software flow control
        rtscts=False,  # No hardware flow control
        dsrdtr=False  # No DSR/DTR flow control
    )
    # Clear any pending data
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def write_line(ser: serial.Serial, msg: str) -> None:
    """Write a line to serial port with error handling."""
    if not ser.is_open:
        raise serial.SerialException("Serial port is not open")
    line = (msg.strip() + "\n").encode("utf-8")
    try:
        ser.write(line)
        ser.flush()
    except serial.SerialTimeoutException:
        print("Warning: Write timeout - command may not have been sent", file=sys.stderr)
    except serial.SerialException as e:
        print(f"Error writing to serial: {e}", file=sys.stderr)
        raise


def read_available(ser: serial.Serial, timeout: float = 0.5, max_lines: int = 50) -> List[str]:
    """Read available lines from serial port with timeout and line limit."""
    lines: List[str] = []
    if not ser.is_open:
        return lines
    
    try:
        # Read with timeout, but limit number of lines to avoid backlog
        start_time = time.time()
        last_data_time = start_time
        
        while time.time() - start_time < timeout and len(lines) < max_lines:
            if ser.in_waiting > 0:
                data = ser.readline()
                if data:
                    line = data.decode(errors="ignore").rstrip()
                    if line:
                        lines.append(line)
                        last_data_time = time.time()
                else:
                    # No more data available right now
                    # If we haven't received data for a while, stop
                    if time.time() - last_data_time > 0.1:
                        break
            else:
                # If no data waiting and we've read some, check if we should stop
                if len(lines) > 0 and time.time() - last_data_time > 0.05:
                    break
                # Small sleep to avoid busy waiting
                time.sleep(0.01)
    except serial.SerialException as e:
        print(f"Error reading from serial: {e}", file=sys.stderr)
    except Exception as e:
        print(f"Unexpected error reading serial: {e}", file=sys.stderr)
    
    return lines


def to_mode(value: str) -> int:
    v = value.lower()
    if v in ("off", "0"):
        return 0
    if v in ("on", "1"):
        return 1
    if v in ("auto", "2"):
        return 2
    raise ValueError("lights mode must be off|on|auto")


def run_demo(ser: serial.Serial, delay_s: float = 0.02) -> None:
    """
    Demo: Simulate zigzag pattern
    - Arm the system first
    - Start at max speed (255) going forward
    - Perform fast zigzag pattern (left-right-left-right)
    - Stop at the end
    """
    print("Starting zigzag demo...")
    
    # Arm the system (ARMED transitions to RUNNING automatically)
    print("-> M:SYS_ARM:0")
    write_line(ser, "M:SYS_ARM:0")
    time.sleep(delay_s * 2)
    
    # Start with max speed forward
    print("-> C:SET_SPEED:255")
    write_line(ser, "C:SET_SPEED:255")
    time.sleep(delay_s)
    
    # Steering angles (matching hardware.h: 50=right, 105=center, 160=left)
    SERVO_CENTER = 105
    SERVO_RIGHT = 50   # Right turn (lower value)
    SERVO_LEFT = 160   # Left turn (higher value)
    
    # Zigzag pattern: left -> right -> left -> right (faster)
    print("Performing zigzag pattern...")
    zigzag_cycles = 4  # Number of left-right cycles
    
    for cycle in range(zigzag_cycles):
        # Turn left (higher value = left)
        print(f"Cycle {cycle + 1}: Turning LEFT")
        for angle in range(SERVO_CENTER, SERVO_LEFT + 1, 2):  # Step by 2 for faster movement
            cmd = f"C:SET_STEER:{angle}"
            write_line(ser, cmd)
            time.sleep(delay_s)
        
        # Turn right (lower value = right)
        print(f"Cycle {cycle + 1}: Turning RIGHT")
        for angle in range(SERVO_LEFT, SERVO_RIGHT - 1, -2):  # Step by 2 for faster movement
            cmd = f"C:SET_STEER:{angle}"
            write_line(ser, cmd)
            time.sleep(delay_s)
        
        # Return to center before next cycle
        if cycle < zigzag_cycles - 1:  # Don't center on last cycle
            for angle in range(SERVO_RIGHT, SERVO_CENTER + 1, 2):
                cmd = f"C:SET_STEER:{angle}"
                write_line(ser, cmd)
                time.sleep(delay_s)
        
        # Read any responses
        responses = read_available(ser, timeout=0.1)
        for ln in responses:
            if ln and "STATUS:" not in ln:  # Filter STATUS messages
                print(f"<- {ln}")
    
    # Return to center
    print("Returning to center...")
    cmd = f"C:SET_STEER:{SERVO_CENTER}"
    write_line(ser, cmd)
    time.sleep(delay_s * 2)
    
    # Stop
    print("-> E:BRAKE_NOW:0")
    write_line(ser, "E:BRAKE_NOW:0")
    time.sleep(delay_s)
    
    print("Zigzag demo completed!")


def interactive_loop(ser: serial.Serial) -> None:
    print("Interactive UART simulator. Type 'help' for commands. Ctrl+C to quit.")
    print(HELP_TEXT)
    while True:
        try:
            raw = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not raw:
            # show any pending output (filter STATUS messages)
            responses = read_available(ser, timeout=0.2)
            status_messages = [ln for ln in responses if "STATUS:" in ln]
            other_messages = [ln for ln in responses if "STATUS:" not in ln]
            
            for ln in other_messages:
                if ln:
                    print(f"<- {ln}")
            
            if status_messages:
                print(f"<- {status_messages[-1]}")
                if len(status_messages) > 1:
                    print(f"    ... ({len(status_messages) - 1} more STATUS messages suppressed)")
            continue
        parts = raw.split()
        cmd = parts[0].lower()
        try:
            if cmd in ("quit", "exit"):
                break
            elif cmd == "help":
                print(HELP_TEXT)
            elif cmd == "speed":
                if len(parts) != 2:
                    print("Usage: speed <0-255>")
                    continue
                val = int(parts[1])
                if not (0 <= val <= 255):
                    print("Value must be 0..255")
                    continue
                msg = f"C:SET_SPEED:{val}"
                print(f"-> {msg}")
                write_line(ser, msg)
            elif cmd == "steer":
                if len(parts) != 2:
                    print("Usage: steer <50-160> (50=right, 105=center, 160=left)")
                    continue
                val = int(parts[1])
                if not (50 <= val <= 160):
                    print("Value must be 50..160 (50=right, 105=center, 160=left)")
                    continue
                msg = f"C:SET_STEER:{val}"
                print(f"-> {msg}")
                write_line(ser, msg)
            elif cmd == "lights":
                if len(parts) != 2:
                    print("Usage: lights <off|on|auto>")
                    continue
                mode = to_mode(parts[1])
                print("(note) Lights over UART not handled by firmware; command skipped")
            elif cmd == "emergency":
                msg = "E:BRAKE_NOW:0"
                print(f"-> {msg}")
                write_line(ser, msg)
            elif cmd == "stop":
                msg = "C:SET_SPEED:0"
                print(f"-> {msg}")
                write_line(ser, msg)
            elif cmd == "arm":
                msg = "M:SYS_ARM:0"
                print(f"-> {msg}")
                write_line(ser, msg)
                print("(note) System must be ARMED before control commands work")
            elif cmd == "disarm":
                msg = "M:SYS_DISARM:0"
                print(f"-> {msg}")
                write_line(ser, msg)
            elif cmd == "demo":
                run_demo(ser)
            else:
                # Allow raw full protocol lines
                if ":" in raw:
                    print(f"-> {raw}")
                    write_line(ser, raw)
                else:
                    print("Unknown command. Type 'help'.")
            # Wait a bit for response, then read available data
            # Use shorter timeout to avoid accumulating too many STATUS messages
            time.sleep(0.05)  # Give ESP32 time to process
            responses = read_available(ser, timeout=0.2)  # Shorter timeout to avoid backlog
            # Filter out repetitive STATUS messages (keep only the last one if multiple)
            status_messages = [ln for ln in responses if "STATUS:" in ln]
            other_messages = [ln for ln in responses if "STATUS:" not in ln]
            
            # Print other messages immediately
            for ln in other_messages:
                if ln:
                    print(f"<- {ln}")
            
            # Print only the last STATUS message if there are multiple
            if status_messages:
                print(f"<- {status_messages[-1]}")
                if len(status_messages) > 1:
                    print(f"    ... ({len(status_messages) - 1} more STATUS messages suppressed)")
        except ValueError as ve:
            print(f"Error: {ve}")
        except serial.SerialException as se:
            print(f"Serial port error: {se}")
            print("The serial port may be disconnected or in use by another program.")
            print("Make sure PlatformIO Serial Monitor is closed.")
            break
        except KeyboardInterrupt:
            print("\nInterrupted by user")
            break
        except Exception as ex:
            print(f"Unexpected error: {ex}")
            import traceback
            traceback.print_exc()


def send_batch(port: str, baud: int, commands: List[str], delay_s: float) -> None:
    ser = open_serial(port, baud)
    try:
        for cmd in commands:
            print(f"-> {cmd}")
            write_line(ser, cmd)
            time.sleep(delay_s)
            responses = read_available(ser, timeout=0.5)
            for ln in responses:
                if ln:
                    print(f"<- {ln}")
    finally:
        if ser.is_open:
            ser.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 UART command simulator (interactive by default)")
    parser.add_argument("port", nargs="?", help="Serial port (e.g., /dev/ttyUSB0, COM5). If omitted, you'll be prompted.")
    parser.add_argument("command", nargs="*", help="Optional raw command(s) to send and exit")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200 for USB; use 921600 for Serial1)")
    parser.add_argument("--sleep", type=float, default=0.05, help="Delay between commands in seconds")
    parser.add_argument("--non-interactive", action="store_true", help="Do not run REPL; just send provided commands")
    args = parser.parse_args()

    port = args.port or select_port_interactive()

    if args.non_interactive and args.command:
        send_batch(port, args.baud, args.command, args.sleep)
        return

    try:
        ser = open_serial(port, args.baud)
        try:
            interactive_loop(ser)
        finally:
            if ser.is_open:
                ser.close()
                print("\nSerial port closed.")
    except serial.SerialException as se:
        print(f"Failed to open {port}: {se}", file=sys.stderr)
        print("\nTroubleshooting tips:", file=sys.stderr)
        print("  - Make sure the ESP32 is connected", file=sys.stderr)
        print("  - Close PlatformIO Serial Monitor if it's open", file=sys.stderr)
        print("  - Close any other programs using the serial port", file=sys.stderr)
        print("  - Try unplugging and replugging the USB cable", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n\nExiting...")
        sys.exit(0)


if __name__ == "__main__":
    main()


