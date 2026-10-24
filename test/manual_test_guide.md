# Manual Testing Guide

This guide helps you manually test the RC car system functionality when automated tests aren't sufficient or for hardware validation.

> **Actualizado.** Corregido contra el firmware actual: el firmware **no tiene página web ni Wi-Fi**;
> todo se controla por serie (USB a 115200 o UART externo en GPIO 17/16 a 921600). El watchdog es de
> 300 ms en AUTO y 1000 ms en MANUAL, el freno tarda hasta ~50 ms y la placa arranca `DISARMED`
> (solo con `-DBOOT_ARMED`, que hoy no está en `platformio.ini`, arranca `ARMED`/`RUNNING`).
> Referencia: `docs/BRAIN_TEAM_PROTOCOL.md`.

## Prerequisites

- ESP32 board connected via USB
- Serial monitor open (115200 baud)
- Para las pruebas con movimiento, un script que mande comandos (por ejemplo `test/python/test_uart_simulator.py`):
  escribiendo a mano no se llega a alimentar el watchdog (1 s en MANUAL, 300 ms en AUTO)
- Optional: UART device (Jetson or serial terminal) on GPIO 17/16. Los comandos también se pueden mandar por el mismo USB.
- Ningún objeto a menos de 30 cm delante del ultrasonido: si no, el sistema entra en `FAULT` (`EVENT:FAULT_TRIGGERED:ULTRASONIC_OBSTACLE`)

## Test Procedures

### 1. System Initialization Test

**Objective**: Verify system boots correctly and all tasks start

**Steps**:
1. Upload firmware to ESP32
2. Open serial monitor
3. Verify you see:
   ```
   Hardware initialized
   ========================================
   ESP32 RC Car FreeRTOS System Starting...
   ========================================
   [main] Mailboxes initialized
   [main] LinkRxTask created on Core 1, Priority 4
   [main] MotorTask created on Core 0, Priority 4
   [main] SteerTask created on Core 0, Priority 3
   [main] LightsTask created on Core 1, Priority 1
   [main] SupervisorTask created on Core 1, Priority 2
   [main] LinkTxTask created on Core 1, Priority 2
   [main] UltrasonicTask created on Core 0, Priority 5
   [main] All tasks created. FreeRTOS scheduler running...
   [main] System ready!
   EVENT:SYSTEM_READY
   ```
   Intercaladas aparecen las líneas `[XxxTask] ... task started` y el estado inicial
   (`EVENT:STATE_CHANGED:ARMED` con `-DBOOT_ARMED`, `DISARMED` sin él, más `EVENT:MODE_CHANGED:MANUAL`).
   Cada línea llega entera: si ves dos eventos pegados en una línea, la placa tiene un firmware
   anterior a b4acb21.

**Expected Result**: All tasks start without errors and the built-in LED (GPIO 2) turns on

---

### 2. ARM/DISARM Test

**Objective**: Verify system state machine transitions

**Steps**:
1. Send `C:SET_SPEED:150`: con la placa desarmada se ignora (`[MotorTask] SET_SPEED ignored - system DISARMED`)
2. Send `M:SYS_ARM:0`
3. Verify state changes to `RUNNING` (en MANUAL, `ARMED` pasa solo a `RUNNING`)
4. Send `M:SYS_DISARM:0` and verify state changes back to `DISARMED`

**Expected Result**: 
- State transitions work correctly (`EVENT:STATE_CHANGED:<estado>`)
- Serial monitor shows: `[SupervisorTask] System ARMED` / `[SupervisorTask] System DISARMED`

---

### 3. Motor Control Test

**Objective**: Verify traction in both directions

**Steps**:
1. ARM the system
2. Send `C:SET_SPEED:150` (por encima de ~140 para que arranque) y mantener el latido (`M:PING:0` o el mismo comando a 10 Hz)
3. Verify motor moves forward
4. Send `C:SET_SPEED:0`: el motor para
5. Send `C:SET_DIR:0` y luego `C:SET_SPEED:150`
6. Verify motor moves backward (con 60 ms de tiempo muerto al invertir)
7. Send `C:SET_SPEED:0` y `C:SET_DIR:1`

**Expected Result**:
- Motor responds in both directions
- Motor stops with speed 0
- Reverse lights turn on when going backward

---

### 4. Steering Control Test

**Objective**: Verify steering control

**Steps**:
1. System ARMED
2. Send `C:SET_STEER:50` y verificar que gira a la izquierda
3. Send `C:SET_STEER:160` y verificar que gira a la derecha
4. Send `C:SET_STEER:105` y verificar que vuelve al centro

**Expected Result**: Steering follows the command (50 izquierda, 105 centro, 160 derecha)

---

### 5. Speed Control Test

**Objective**: Verify speed changes with the commanded value

**Steps**:
1. System ARMED
2. Send `C:SET_SPEED:150` and note motor speed
3. Send `C:SET_SPEED:255` and note motor speed (should be faster)
4. Probar por debajo de ~140 desde parado: el firmware acepta el valor pero el motor no arranca (fricción estática)

**Expected Result**: Motor speed changes based on the commanded value

---

### 6. Lights Control Test

**Objective**: Verify lights work in all modes

**Steps**:
1. Send `M:LIGHTS_ON:0` and verify headlights turn on
2. Send `M:LIGHTS_OFF:0` and verify headlights turn off
3. Send `M:LIGHTS_AUTO:0`
4. Cover LDR sensor (GPIO 35) and verify headlights turn on
5. Uncover LDR sensor and verify headlights turn off

**Expected Result**: Lights respond to all control modes

---

### 7. Emergency Brake Test

**Objective**: Verify emergency brake functionality

**Steps**:
1. System ARMED
2. Motor running forward
3. Send `E:BRAKE_NOW:0`
4. Verify motor stops (hasta ~50 ms, no < 1 ms)

**Expected Result**: Motor stops (`EVENT:CMD_EXECUTED:EMERGENCY_BRAKE`) y queda bloqueado 5 s
aunque llegue un `C:SET_SPEED` nuevo. El estado no cambia.

---

### 8. E-STOP GPIO Test

**Objective**: Verify hardware E-STOP functionality

**Steps**:
1. System in RUNNING state
2. Motor running
3. Connect GPIO 4 to GND (simulating E-STOP press)
4. Verify motor stops immediately
5. Verify state changes to FAULT
6. Disconnect GPIO 4 from GND
7. Verify E-STOP releases

**Expected Result**: 
- Motor stops immediately on E-STOP
- State changes to FAULT
- Serial shows: `EVENT:ESTOP_TRIGGERED:GPIO` y `[SupervisorTask] E-STOP triggered via GPIO!`
- Al liberar: `EVENT:ESTOP_RELEASED`. El sistema **sigue** en FAULT hasta volver a armar

---

### 9. Mode Switching Test

**Objective**: Verify MANUAL ↔ AUTO mode switching

**Steps**:
1. System in MANUAL mode
2. Send `M:SYS_MODE:1`
3. Verify `EVENT:MODE_CHANGED:AUTO`
4. Send `M:SYS_MODE:0`
5. Verify `EVENT:MODE_CHANGED:MANUAL`

**Expected Result**: Mode switches correctly. Mandar el modo que ya está solo
produce `EVENT:CMD_RECEIVED:SYS_MODE:<modo>`.

---

### 10. UART Communication Test (AUTO Mode)

**Objective**: Verify UART communication with Jetson

**Prerequisites**: UART device connected to GPIO 17 (TX) and GPIO 16 (RX) at 921600, o por USB a 115200.
Usar un script: en AUTO el watchdog salta a los 300 ms sin comandos.

**Steps**:
1. Send `M:SYS_MODE:1\n` (AUTO), esperar ~100 ms
2. Send UART message: `M:SYS_ARM:0\n`
3. Verify system ARMs (y pasa a RUNNING)
4. Send: `C:SET_SPEED:150\n`
5. Verify motor speed changes y se mantiene (latching) mientras se manda `M:PING:0\n` cada 100 ms
6. Send: `C:SET_STEER:90\n`
7. Verify steering moves
8. Send: `E:BRAKE_NOW:0\n`
9. Verify emergency brake triggers

**Expected Result**: All UART commands are parsed and executed correctly

---

### 11. Watchdog Timeout Test

**Objective**: Verify watchdog timeout in AUTO mode

**Steps**:
1. Set mode to AUTO
2. ARM the system
3. Start sending heartbeat messages via UART (`M:PING:0` every 100 ms)
4. Stop sending messages
5. Wait > 300 ms (en MANUAL, > 1000 ms)
6. Verify system enters FAULT state
7. Verify motor stops and steering centers

**Expected Result**: 
- System detects heartbeat timeout
- Enters FAULT state
- Serial shows: `EVENT:WATCHDOG_TIMEOUT` y `[SupervisorTask] Watchdog timeout! Heartbeat age: XXX ms`
- No salta justo después de armar (era un watchdog falso, corregido en b4acb21)

---

## Test Checklist

- [ ] System initialization
- [ ] ARM/DISARM functionality
- [ ] Motor control (forward/back)
- [ ] Steering control (left/right)
- [ ] Speed control
- [ ] Lights control (ON/OFF/AUTO)
- [ ] Emergency brake
- [ ] E-STOP GPIO
- [ ] Mode switching
- [ ] UART communication (if hardware available)
- [ ] Watchdog timeout

## Troubleshooting

### Motor not responding
- Verify system is ARMED/RUNNING and not in FAULT (watchdog, E-STOP, ultrasonido)
- Check there is no 5 s cooldown after a brake
- Speed above ~140
- Verify GPIO connections (IN3, IN4, ENB)
- Check serial monitor for error messages

### UART not working
- Verify baud rate is 921600 (UART externo) / 115200 (USB)
- Check GPIO 17 (TX) and GPIO 16 (RX) connections
- Verify UART device is configured correctly
- Check serial monitor for parsing errors
