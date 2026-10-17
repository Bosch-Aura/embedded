# Manual Testing Guide

This guide helps you manually test the RC car system functionality when automated tests aren't sufficient or for hardware validation.

> **Actualizado.** Corregido contra el firmware actual: la página web ya no tiene botones
> adelante/atrás/izquierda/derecha ni selector de modo (tiene ARMAR / DESARMAR / E-STOP, luces,
> slider de velocidad y volante), el UART externo va en GPIO 17/16, el watchdog es de 300 ms en AUTO
> y 1000 ms en MANUAL, el freno tarda hasta ~50 ms y, con `-DBOOT_ARMED` (hoy en `platformio.ini`),
> la placa arranca `ARMED`/`RUNNING` en vez de `DISARMED`. Referencia: `docs/BRAIN_TEAM_PROTOCOL.md`.

## Prerequisites

- ESP32 board connected via USB
- Serial monitor open (115200 baud)
- Wi-Fi capable device (phone/laptop) for web interface testing
- Optional: UART device (Jetson or serial terminal) for AUTO mode testing. Los comandos también se pueden mandar por el mismo USB.
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
   [main] WebTask created on Core 1, Priority 2
   [main] UltrasonicTask created on Core 0, Priority 5
   [main] All tasks created. FreeRTOS scheduler running...
   [main] System ready!
   EVENT:SYSTEM_READY
   ```
   Intercaladas aparecen las líneas `[XxxTask] ... task started` y el estado inicial
   (`EVENT:STATE_CHANGED:ARMED` con `-DBOOT_ARMED`, `DISARMED` sin él, más `EVENT:MODE_CHANGED:MANUAL`).
   Cada línea llega entera: si ves dos eventos pegados en una línea, la placa tiene un firmware
   anterior a b4acb21.

**Expected Result**: All tasks start without errors

---

### 2. Wi-Fi AP Test

**Objective**: Verify Wi-Fi Access Point is created

**Steps**:
1. Check available Wi-Fi networks on your device
2. Look for network named "RC-Car-ESP32"
3. Connect to it (no password)

**Expected Result**: 
- Wi-Fi network visible
- Can connect successfully
- Serial monitor shows: `[WebTask] Wi-Fi AP started. SSID: RC-Car-ESP32, IP: 192.168.4.1`

---

### 3. Web Interface Test

**Objective**: Verify web server responds and HTML loads

**Steps**:
1. Connect to Wi-Fi AP "RC-Car-ESP32"
2. Open browser and navigate to `http://192.168.4.1`
3. Verify you see the control interface with:
   - ARMAR / DESARMAR buttons
   - E-STOP button (freno de emergencia)
   - Status line showing state and mode (`Estado: <estado> / <modo>`)
   - Lights controls (off / on / auto)
   - Vertical speed slider (-255 a 255: arriba adelante, abajo atrás)
   - Steering wheel

**Expected Result**: Web page loads completely, all buttons visible

---

### 4. Manual Mode - ARM/DISARM Test

**Objective**: Verify system state machine transitions

**Steps**:
1. Open web interface
2. Check status line: con `-DBOOT_ARMED` muestra `RUNNING` desde el arranque; sin él, `DISARMED`
3. Click "DESARMAR" (si estaba armado) y verificar `DISARMED`
4. Click "ARMAR" button
5. Verify status changes to "RUNNING" (en MANUAL, `ARMED` pasa solo a `RUNNING`)
6. Click "DESARMAR" and verify status changes back to "DISARMED"

**Expected Result**: 
- State transitions work correctly
- Serial monitor shows: `[SupervisorTask] System ARMED` / `[SupervisorTask] System DISARMED`

---

### 5. Manual Mode - Motor Control Test

**Objective**: Verify motor control in MANUAL mode

**Steps**:
1. System in MANUAL mode (el de arranque; la web no tiene selector de modo)
2. ARM the system
3. Subir el slider de velocidad (por encima de ~140 para que arranque)
4. Verify motor moves forward
5. Volver el slider a 0: el motor para
6. Bajar el slider
7. Verify motor moves backward (con 60 ms de tiempo muerto al invertir)
8. Volver el slider a 0

**Expected Result**:
- Motor responds to the slider in both directions
- Motor stops with the slider at 0
- Reverse lights turn on when going backward

---

### 6. Manual Mode - Steering Control Test

**Objective**: Verify steering control in MANUAL mode

**Steps**:
1. System in MANUAL mode and ARMED
2. Girar el volante hacia la izquierda
3. Verify steering turns left
4. Soltar el volante (vuelve al centro, 105)
5. Girar el volante hacia la derecha
6. Verify steering turns right
7. Soltar el volante

**Expected Result**:
- Steering follows the wheel (50 izquierda, 105 centro, 160 derecha)
- Steering centers when the wheel is released

---

### 7. Manual Mode - Speed Control Test

**Objective**: Verify speed slider works

**Steps**:
1. System in MANUAL mode and ARMED
2. Move speed slider to ~150 hacia adelante
3. Note motor speed
4. Move speed slider to maximum (255)
5. Note motor speed (should be faster)
6. Probar por debajo de ~140 desde parado: el firmware acepta el valor pero el motor no arranca (fricción estática)

**Expected Result**: Motor speed changes based on slider position

---

### 8. Lights Control Test

**Objective**: Verify lights work in all modes

**Steps**:
1. Click "Lights On" button
2. Verify headlights turn on
3. Click "Lights Off" button
4. Verify headlights turn off
5. Click "Lights Auto" button
6. Cover LDR sensor (GPIO 35)
7. Verify headlights turn on
8. Uncover LDR sensor
9. Verify headlights turn off

**Expected Result**: Lights respond to all control modes

---

### 9. Emergency Brake Test

**Objective**: Verify emergency brake functionality

**Steps**:
1. System in MANUAL mode and ARMED
2. Motor running forward
3. Click "E-STOP" button
4. Verify motor stops (hasta ~50 ms, no < 1 ms) and the slider goes back to 0

**Expected Result**: Motor stops (`EVENT:CMD_EXECUTED:EMERGENCY_BRAKE`) y queda bloqueado 5 s
aunque se mueva el slider. El estado no cambia.

---

### 10. E-STOP GPIO Test

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

### 11. Mode Switching Test

**Objective**: Verify MANUAL ↔ AUTO mode switching

**Steps**:
La página web no tiene botones de modo; se cambia por serie (o con `http://192.168.4.1/mode?value=AUTO`).

1. System in MANUAL mode
2. Send `M:SYS_MODE:1`
3. Verify status line shows "AUTO" (`EVENT:MODE_CHANGED:AUTO`)
4. Send `M:SYS_MODE:0`
5. Verify status line shows "MANUAL"

**Expected Result**: Mode switches correctly, status updates. Mandar el modo que ya está solo
produce `EVENT:CMD_RECEIVED:SYS_MODE:<modo>`.

---

### 12. UART Communication Test (AUTO Mode)

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

### 13. Watchdog Timeout Test

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

### 14. Status API Test

**Objective**: Verify status endpoint returns correct JSON

**Steps**:
1. Open browser
2. Navigate to `http://192.168.4.1/status`
3. Verify JSON response: `{"mode":"MANUAL","state":"RUNNING"}` con `-DBOOT_ARMED` (`"DISARMED"` sin él)
4. Change mode and state
5. Refresh status endpoint
6. Verify JSON updates

**Expected Result**: Status endpoint returns correct JSON with current mode and state

---

## Test Checklist

- [ ] System initialization
- [ ] Wi-Fi AP creation
- [ ] Web interface loads
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
- [ ] Status API

## Troubleshooting

### Wi-Fi AP not visible
- Check serial monitor for Wi-Fi initialization errors
- Verify board has Wi-Fi capability
- Try power cycling the board

### Web interface not loading
- Verify you're connected to "RC-Car-ESP32" network
- Check IP address in serial monitor
- Try accessing `http://192.168.4.1` directly

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

