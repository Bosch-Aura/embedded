# Quick UART Simulation Guide

> **Actualizado.** Esta guía describía un simulador con flags que no existen (`-p`, `--demo`,
> `--list-ports`), el UART externo en GPIO 9/10, un watchdog de 120 ms y un freno "< 1 ms".
> Las secciones de abajo están corregidas contra el firmware actual; la referencia del protocolo
> es `docs/BRAIN_TEAM_PROTOCOL.md`. Usá el simulador de `test/python/`; el de la raíz de `test/` es
> una versión vieja que manda `CONTROL:...`/`LIGHTS:...`.

## Method 1: Python Simulator (Easiest)

### Step 1: Install Dependencies

```bash
pip install -r test/python/requirements.txt
```

### Step 2: Find Your Serial Port

Si se corre el simulador **sin puerto**, lista los que encuentra y pregunta cuál usar.

**Linux:**
```bash
ls /dev/ttyUSB* /dev/ttyACM* /dev/ttyCH341USB*
```
En la **Jetson**, con el driver vendor de WCH, la placa aparece como `/dev/ttyCH341USB0`
(no `/dev/ttyUSB0`). Además, **abrir el puerto reinicia la placa** (vuelve a llegar
`EVENT:SYSTEM_READY`), aunque DTR/RTS queden en reposo: esperá ese evento antes de mandar nada.

**Windows:** COM3, COM4, COM5, etc.

**macOS:** `/dev/tty.usbserial-*`, `/dev/cu.usbserial-*`

### Step 3: Connect Hardware

**No hace falta adaptador**: el firmware lee comandos también por el USB (115200 baud), que es lo
más simple. El adaptador solo hace falta para probar el UART externo:

- **Adapter TX** → **ESP32 GPIO 16** (RX pin)
- **Adapter RX** → **ESP32 GPIO 17** (TX pin)
- **Adapter GND** → **ESP32 GND**

**Note:** ESP32 has TWO serial ports:
- **USB Serial** (115200): programación, depuración **y** comandos. Por acá salen todos los `EVENT:`.
- **UART1** (921600, para la Jetson por cable): GPIO 17 (TX), GPIO 16 (RX). Por acá solo vuelven
  `EVENT:STATE_CHANGED` y `EVENT:MODE_CHANGED`. (Antes estaba en GPIO 9/10, que son del flash SPI.)

### Step 4: Run the Simulator

**Interactive Mode:**
```bash
python3 test/python/test_uart_simulator.py /dev/ttyUSB0
# Reemplazá /dev/ttyUSB0 por tu puerto (/dev/ttyCH341USB0 en la Jetson).
# Por el UART externo: --baud 921600
```

**Demo Mode:** dentro del simulador, escribí `demo` (zigzag con el motor a 255).

**Comandos sueltos, sin REPL:**
```bash
python3 test/python/test_uart_simulator.py /dev/ttyUSB0 --non-interactive M:SYS_ARM:0 C:SET_SPEED:150
```

### Step 5: Use Commands

Once in interactive mode, type commands:

```
arm              # M:SYS_ARM:0
mode auto        # M:SYS_MODE:1  (mode manual -> M:SYS_MODE:0)
speed 150        # C:SET_SPEED:150 (latching; por debajo de ~140 el motor no arranca)
steer 90         # C:SET_STEER:90 (50 izquierda, 105 centro, 160 derecha)
emergency        # E:BRAKE_NOW:0
stop             # C:SET_SPEED:0
disarm           # M:SYS_DISARM:0
M:PING:0         # cualquier línea con ':' se manda tal cual
quit             # Exit
```

**Watchdog:** el simulador **no** manda latidos. Si pasa más de 1 s (MANUAL) o 300 ms (AUTO) sin
ningún comando válido, el sistema entra en `FAULT`, frena y hay que volver a armar. Escribiendo a
mano eso pasa casi siempre; para probar con el auto andando, mandá `M:PING:0` desde un script.

## Method 2: Serial Terminal (Manual)

### Using screen (Linux/Mac)

```bash
# Install if needed: sudo apt install screen
screen /dev/ttyUSB0 115200      # por USB
screen /dev/ttyUSB1 921600      # por un adaptador en el UART externo
```

Then type commands manually:
```
M:SYS_ARM:0
M:SYS_MODE:1
C:SET_SPEED:150
C:SET_STEER:90
E:BRAKE_NOW:0
```

Press `Ctrl+A` then `K` to exit screen.

### Using minicom (Linux)

```bash
# Install: sudo apt install minicom
minicom -D /dev/ttyUSB0 -b 115200   # 921600 si es el UART externo
```

### Using PuTTY (Windows)

1. Open PuTTY
2. Connection type: Serial
3. Serial line: COM3 (your port)
4. Speed: 115200 (USB) o 921600 (UART externo)
5. Click Open
6. Type commands directly

### Using Arduino Serial Monitor

**Note:** por USB (115200) sirve cualquier monitor. Arduino Serial Monitor puede no soportar 921600 para el UART externo.

## Method 3: Simple Python Script

Create a file `test_uart_simple.py`:

```python
import serial
import time

ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)  # /dev/ttyCH341USB0 en la Jetson
while b'EVENT:SYSTEM_READY' not in ser.readline():   # abrir el puerto reinicia la placa
    pass

# Switch to AUTO
ser.write(b'M:SYS_MODE:1\n')
time.sleep(0.1)          # no mandar dos M: pegados

# ARM system
ser.write(b'M:SYS_ARM:0\n')
time.sleep(0.1)

# Set speed (latching: queda sostenida)
ser.write(b'C:SET_SPEED:150\n')

# Set steering
ser.write(b'C:SET_STEER:90\n')

# Mantener vivo el enlace 2 s (AUTO: FAULT a los 300 ms sin comandos)
for _ in range(20):
    ser.write(b'M:PING:0\n')
    time.sleep(0.1)

ser.write(b'E:BRAKE_NOW:0\n')

# Read response
if ser.in_waiting:
    print(ser.read(ser.in_waiting).decode())

ser.close()
```

Run: `python3 test_uart_simple.py`

## Testing Scenarios

### Scenario 1: Basic Communication Test

```bash
# Por USB alcanza con el simulador: muestra él mismo lo que responde el ESP32.
# (pio device monitor y el simulador no pueden abrir el mismo puerto a la vez.)
python3 test/python/test_uart_simulator.py /dev/ttyUSB0
```

In simulator (en MANUAL, que es el modo de arranque):
```
> arm
> speed 150
```

**Expected** (entre otras líneas de depuración `[Tarea] ...`):
```
EVENT:CMD_RECEIVED:SYS_ARM
EVENT:CMD_EXECUTED:SYS_ARM
EVENT:CMD_RECEIVED:SET_SPEED:150
EVENT:CMD_EXECUTED:SET_SPEED:150
```
Si ya estaba armado (con `-DBOOT_ARMED` arranca así), igual llega `CMD_EXECUTED:SYS_ARM`. Si entre
`arm` y `speed` pasó más de 1 s, antes llega `EVENT:WATCHDOG_TIMEOUT` y la velocidad se ignora
(ver Scenario 2). En AUTO el margen es de 300 ms: a mano no da, hace falta un script.

### Scenario 2: Heartbeat Test

El simulador **no** manda latidos solo, así que alcanza con dejar de escribir:

1. Armar y mandar un comando (`arm`, `speed 150`)
2. No mandar nada durante más de 1 s (MANUAL) o 300 ms (AUTO)
3. Apretar Enter en una línea vacía para ver lo que llegó

**Expected:**
```
EVENT:WATCHDOG_TIMEOUT
[SupervisorTask] Watchdog timeout! Heartbeat age: XXX ms
EVENT:CMD_EXECUTED:EMERGENCY_BRAKE
EVENT:STATE_CHANGED:FAULT
```
`XXX` tiene que ser un poco más que el timeout. Un número cercano a 4294967296 era el watchdog
falso al armar, corregido en b4acb21: si aparece, la placa tiene firmware viejo.

### Scenario 3: Emergency Brake Test

```bash
python3 test/python/test_uart_simulator.py /dev/ttyUSB0
```

```
> arm
> speed 200
> emergency
```

**Expected:** `EVENT:CMD_RECEIVED:BRAKE_NOW` y `EVENT:CMD_EXECUTED:EMERGENCY_BRAKE`; el motor para
en hasta ~50 ms (no < 1 ms) y queda bloqueado 5 s. Después sigue parado hasta un `speed` nuevo.
(Hay que escribir rápido: si pasa más de 1 s entre comandos, el watchdog llega primero.)

## Hardware Setup Diagram

```
ESP32                    USB-to-Serial Adapter
------                   --------------------
GPIO 17 (TX) -------->  RX
GPIO 16 (RX) <--------  TX
GND     ------------  GND

USB-to-Serial Adapter → Computer USB Port
```

## Troubleshooting

### "No such file or directory" (Linux)

The port might need permissions:
```bash
sudo chmod 666 /dev/ttyUSB0
# Or add user to dialout group:
sudo usermod -a -G dialout $USER
# Then logout/login
```

### "Permission denied"

```bash
sudo chmod 666 /dev/ttyUSB0
```

### "Serial port already in use"

Another program is using the port. Close other serial terminals or IDE serial monitors.

### No response from ESP32

1. **Check connections:** TX→RX, RX→TX (crossed!)
2. **Check baud rate:** 115200 por USB, 921600 por el UART externo
3. **Check ESP32 is running:** Look for serial debug output
4. **Check UART pins:** GPIO 17 (TX) / 16 (RX). Por el UART externo solo vuelven `STATE_CHANGED`/`MODE_CHANGED`
5. **Check state:** si los comandos de control no hacen nada, mirá si el sistema está en `FAULT` (watchdog o ultrasonido: cualquier objeto a < 30 cm del sensor lo dispara)

### Wrong port

List all ports (el simulador sin argumentos lista los puertos y pregunta):
```bash
python3 test/python/test_uart_simulator.py
```

Or manually:
```bash
# Linux (Jetson con driver WCH: /dev/ttyCH341USB*)
ls /dev/ttyUSB* /dev/ttyACM* /dev/ttyCH341USB*

# Windows
# Check Device Manager → Ports (COM & LPT)

# macOS
ls /dev/tty.usbserial* /dev/cu.usbserial*
```

## Quick Reference Commands

| Command | Format | Example |
|---------|--------|---------|
| ARM | `M:SYS_ARM:0` | `arm` |
| DISARM | `M:SYS_DISARM:0` | `disarm` |
| AUTO mode | `M:SYS_MODE:1` | `mode auto` |
| MANUAL mode | `M:SYS_MODE:0` | `mode manual` |
| Set speed | `C:SET_SPEED:VALUE` | `speed 150` |
| Set steering | `C:SET_STEER:ANGLE` (50-160) | `steer 90` |
| Set direction | `C:SET_DIR:0\|1` | `C:SET_DIR:0` |
| Heartbeat | `M:PING:0` | `M:PING:0` |
| Emergency brake | `E:BRAKE_NOW:0` | `emergency` |
| Stop (alias del freno) | `E:STOP:0` | `E:STOP:0` |
| Speed 0 | `C:SET_SPEED:0` | `stop` |

## Example Test Session

```bash
# Start Python simulator
$ python3 test/python/test_uart_simulator.py /dev/ttyUSB0

> arm
-> M:SYS_ARM:0

> mode auto
-> M:SYS_MODE:1

> speed 150
-> C:SET_SPEED:150

> steer 90
-> C:SET_STEER:90

> emergency
-> E:BRAKE_NOW:0

> disarm
-> M:SYS_DISARM:0

> quit
```

## Tips

1. **Use two terminals** solo si el simulador va por el UART externo: por USB, el simulador ya muestra la salida y el monitor no puede abrir el mismo puerto
2. **Start slow:** Test one command at a time
3. **Watch serial monitor:** See how ESP32 parses commands
4. **Heartbeat:** el simulador **no** lo manda solo; sin comandos válidos durante 1 s (MANUAL) / 300 ms (AUTO) el sistema entra en `FAULT`
5. **Test mode switching:** Try MANUAL ↔ AUTO switching

