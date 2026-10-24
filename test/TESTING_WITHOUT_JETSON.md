# Testing ESP32 RC Car Without Jetson

This guide explains how to test the AUTO mode and UART communication without a Jetson board.

> **Actualizado.** Esta guía usaba flags del simulador que no existen (`-p`, `--demo`,
> `--list-ports`), el UART externo en GPIO 9/10, un watchdog de 120 ms "solo en AUTO" y telemetría
> de estado periódica que el firmware ya no manda. Está corregida contra el firmware actual; la
> referencia del protocolo es `docs/BRAIN_TEAM_PROTOCOL.md`. Puntos que cambian cómo se prueba:
>
> - **Control *latching***: un `C:SET_SPEED` queda sostenido; vencido el TTL de 200 ms el motor no frena.
> - **Watchdog en los dos modos**: 300 ms en AUTO, 1000 ms en MANUAL. Sin ningún comando válido en
>   ese tiempo, `FAULT` + freno. `M:PING:0` lo alimenta sin hacer nada más.
> - **Arranque**: arranca `DISARMED` (así está hoy `platformio.ini`); con `-DBOOT_ARMED`, `ARMED`.
> - **Ultrasonido activo**: un objeto a menos de 30 cm lleva el sistema a `FAULT`.

## Methods

### Method 1: Python UART Simulator (Recommended)

Use the provided Python script to simulate Jetson communication.

#### Prerequisites

```bash
pip install -r test/python/requirements.txt
```

#### Usage

Usá `test/python/test_uart_simulator.py` (el de la raíz de `test/` es una versión vieja). Por
defecto va a 115200, o sea por **USB**: no hace falta adaptador.

1. **List available serial ports:** correr el simulador sin puerto; lista los que hay y pregunta.

2. **Interactive mode:**
   ```bash
   python3 test/python/test_uart_simulator.py /dev/ttyUSB0
   # Jetson (driver WCH): /dev/ttyCH341USB0  -- abrir el puerto reinicia la placa
   # On Windows: python3 test/python/test_uart_simulator.py COM3
   # UART externo: agregar --baud 921600
   ```

3. **Run demo sequence:** dentro del simulador, escribir `demo`.

4. **Comandos sueltos:** `--non-interactive` seguido de los comandos, separados por `--sleep` segundos.

#### Interactive Commands

- `arm` - `M:SYS_ARM:0`
- `disarm` - `M:SYS_DISARM:0`
- `mode auto` / `mode manual` - `M:SYS_MODE:1` / `M:SYS_MODE:0`
- `speed <0-255>` - `C:SET_SPEED` (e.g., `speed 150`; por debajo de ~140 el motor no arranca)
- `steer <angle>` - `C:SET_STEER`, 50-160 (50 izquierda, 105 centro, 160 derecha)
- `emergency` - `E:BRAKE_NOW:0`
- `stop` - `C:SET_SPEED:0`
- cualquier línea con `:` - se manda tal cual (p. ej. `M:PING:0`, `C:SET_DIR:0`)
- `quit` - Exit

No hay comando de heartbeat ni latido automático: el simulador no alimenta el watchdog solo.

### Method 2: Serial Terminal (Manual Testing)

Use any serial terminal program to send commands manually.

#### Setup

Por **USB** alcanza con abrir el puerto a **115200**. Para probar el **UART externo**:

1. **Connect ESP32:**
   - UART TX (GPIO 17) → Connect to your serial adapter RX
   - UART RX (GPIO 16) → Connect to your serial adapter TX
   - GND → Common ground

2. **Configure serial terminal:**
   - Baud rate: **921600** (UART externo) / 115200 (USB)
   - Data bits: 8
   - Stop bits: 1
   - Parity: None
   - Flow control: None

#### Recommended Serial Terminals

- **Linux/Mac:** `screen`, `minicom`, `picocom`
  ```bash
  screen /dev/ttyUSB0 921600
  # Or
  picocom -b 921600 /dev/ttyUSB0
  ```

- **Windows:** PuTTY, Tera Term, or Arduino Serial Monitor (set baud to 921600)

- **Cross-platform:** 
  - [Serial Monitor](https://github.com/Serial-Studio/Serial-Studio)
  - [CoolTerm](https://freeware.the-meiers.org/)

#### Sending Commands

Type commands and press Enter. Each command should end with a newline:

```
M:SYS_ARM:0
C:SET_SPEED:150
C:SET_STEER:90
E:BRAKE_NOW:0
```

La lista completa de comandos está en `docs/BRAIN_TEAM_PROTOCOL.md`.

### Method 3: Arduino/ESP32 Serial Bridge

Use a second Arduino or ESP32 to send commands programmatically.

#### Arduino Example

```cpp
void setup() {
  Serial.begin(115200);  // USB Serial
  Serial1.begin(921600, SERIAL_8N1, RX_PIN, TX_PIN);  // pines del puente; cruzar con GPIO 17 (TX) / 16 (RX) del ESP32
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    Serial1.println(cmd);  // Forward to ESP32
  }
  
  if (Serial1.available()) {
    String response = Serial1.readStringUntil('\n');
    Serial.println("ESP32: " + response);  // Echo back
  }
}
```

## Test Scenarios

### Scenario 1: Basic UART Communication

**Goal:** Verify UART parsing and command routing

**Steps:**
1. Connect serial terminal to ESP32 (USB 115200, or UART GPIO 17/16 at 921600)
2. Open serial monitor
3. Send: `M:SYS_ARM:0`
4. Verify serial monitor shows: `EVENT:CMD_RECEIVED:SYS_ARM` y `EVENT:CMD_EXECUTED:SYS_ARM`
5. Send: `C:SET_SPEED:150` (en menos de 1 s, o salta el watchdog)
6. Verify motor responds: `EVENT:CMD_EXECUTED:SET_SPEED:150`

**Expected:** Commands are parsed and routed correctly

### Scenario 2: AUTO Mode Control

**Goal:** Test AUTO mode with UART commands

**Steps:**
1. Use un script (a mano no se llega a los 300 ms del watchdog de AUTO)
2. Send: `M:SYS_MODE:1` (AUTO mode), esperar ~100 ms
3. Send: `M:SYS_ARM:0`
4. Send: `C:SET_SPEED:150`
5. Send: `C:SET_STEER:90`
6. Send `M:PING:0` every 100 ms
7. Verify motor and steering respond and hold their values (latching)

**Expected:** System responds to UART commands in AUTO mode

### Scenario 3: Watchdog Timeout

**Goal:** Test heartbeat timeout detection

**Steps:**
1. Switch to AUTO mode: `M:SYS_MODE:1`
2. ARM system: `M:SYS_ARM:0`
3. Send commands for a few seconds (heartbeat, p. ej. `M:PING:0` cada 100 ms)
4. **Stop sending commands for >300 ms** (>1000 ms en MANUAL)
5. Wait and observe

**Expected:** 
- Serial monitor shows: `EVENT:WATCHDOG_TIMEOUT` y `[SupervisorTask] Watchdog timeout! Heartbeat age: XXX ms`
- System enters FAULT state (`EVENT:STATE_CHANGED:FAULT`)
- Motor stops automatically (`EVENT:CMD_EXECUTED:EMERGENCY_BRAKE`) and steering centers
- **No** debe saltar justo después del `SYS_ARM`: ese era un watchdog falso corregido en b4acb21

### Scenario 4: Emergency Brake

**Goal:** Test emergency brake response time

**Steps:**
1. ARM system and set speed: `M:SYS_ARM:0`, `C:SET_SPEED:200`
2. Send emergency: `E:BRAKE_NOW:0`
3. Measure response time (hasta ~50 ms en el peor caso, no < 1 ms)

**Expected:** Motor stops (`EVENT:CMD_EXECUTED:EMERGENCY_BRAKE`), queda bloqueado 5 s y después
sigue parado hasta un `C:SET_SPEED` nuevo. El estado no cambia.

### Scenario 5: Mode Switching

**Goal:** Test MANUAL ↔ AUTO switching

**Steps:**
1. Start in MANUAL mode (default)
2. Switch to AUTO: `M:SYS_MODE:1` (`EVENT:CMD_EXECUTED:SYS_MODE:AUTO` + `EVENT:MODE_CHANGED:AUTO`)
3. Send UART commands (should work, a 10 Hz)
4. Switch back: `M:SYS_MODE:0`
5. Send UART commands again (should work, al menos 1 por segundo)

**Expected:** Both modes work. Los comandos serie se aceptan en los dos modos; lo que cambia
es el timeout del watchdog y que en AUTO hace falta `RUNNING`. Mandar `M:SYS_MODE` con el modo que
ya está solo produce `EVENT:CMD_RECEIVED:SYS_MODE:<modo>`.

### Scenario 6: Heartbeat Timing

**Goal:** Verify heartbeat keeps system alive

**Steps:**
1. AUTO mode + ARMED
2. Send command every 50ms (faster than the 300 ms timeout)
3. System should stay in RUNNING state
4. Stop sending for 400ms
5. System should enter FAULT

**Expected:** Heartbeat timing works correctly

## Troubleshooting

### No Response from ESP32

- **Check connections:** Verify TX/RX are correct (TX→RX, RX→TX)
- **Check baud rate:** 115200 por USB, 921600 por el UART externo
- **Check serial port:** correr el simulador sin puerto para listarlos (en la Jetson: `/dev/ttyCH341USB0`)
- **Check ESP32 logs:** Open second serial monitor for debug output (115200 baud on USB)

### Commands Not Parsed

- **Check format:** Commands must end with `\n` (newline)
- **Check channel:** Use `E:`, `C:`, or `M:` prefix
- **Check serial monitor:** Look for `[LinkRxTask] Failed to parse message`

### Watchdog Always Timing Out

- **Heartbeat frequency:** Send commands at least every 100ms (timeout 300 ms AUTO / 1000 ms MANUAL)
- **Check UART:** Verify commands are actually being received (`EVENT:CMD_RECEIVED:...`); `M:PING:0` no responde nada
- **Check mode:** el watchdog está activo en los **dos** modos
- **Justo después de `SYS_ARM`, con `Heartbeat age` ≈ 4294967xxx ms:** firmware anterior a b4acb21, reflashear

### Motor Not Responding

- **Check state:** AUTO necesita `RUNNING`; MANUAL, `ARMED` o `RUNNING`. En `FAULT` (watchdog, E-STOP o un obstáculo a < 30 cm del ultrasonido) hay que volver a armar
- **Check cooldown:** tras un freno de emergencia, 5 s de bloqueo (`[MotorTask] Speed command ignored (in cooldown)`)
- **Check speed:** por debajo de ~140 el motor no arranca desde parado
- **Check mailbox:** Look for `[MotorTask]` logs in serial monitor

## Quick Test Checklist

- [ ] Serial connection established (115200 USB / 921600 UART externo)
- [ ] UART commands parsed correctly
- [ ] System ARMs via UART
- [ ] AUTO mode activated
- [ ] Speed commands work
- [ ] Steering commands work
- [ ] Emergency brake works
- [ ] Watchdog timeout works
- [ ] Mode switching works
- [ ] State/mode events received (`EVENT:STATE_CHANGED`, `EVENT:MODE_CHANGED`)

## Example Test Session

```bash
# Por USB el simulador ya muestra la salida del ESP32 (no abrir pio device monitor a la vez)
python3 test/python/test_uart_simulator.py /dev/ttyUSB0

# In simulator (rápido: en MANUAL el watchdog salta al segundo sin comandos):
> arm
> speed 150
> steer 90
> speed 200
> steer 70
> emergency
> disarm
> quit
```

## Notes

- **UART pins:** GPIO 17 (TX), GPIO 16 (RX) - different from USB Serial
- **Baud rate:** 921600 en el UART externo, 115200 por USB (los dos aceptan comandos)
- **Command format:** `CHANNEL:COMMAND[:VALUE]` + newline
- **Heartbeat:** cualquier comando **válido** lo alimenta (basura no); timeout 300 ms AUTO / 1000 ms MANUAL
- **Status:** no hay telemetría periódica. Por el UART externo solo salen `EVENT:STATE_CHANGED` y `EVENT:MODE_CHANGED`; todos los eventos salen por USB

