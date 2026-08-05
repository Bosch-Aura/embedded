# Protocolo de Comunicación ESP32 - Guía para el Equipo Brain

Este documento describe cómo enviar comandos desde el sistema Brain (Jetson/PC) hacia el ESP32 que controla los motores, dirección y luces del vehículo.

## ⚠️ IMPORTANTE: Sistema de Seguridad (ARM/DISARM)

**El sistema inicia DESARMADO por defecto.** Debes armar el sistema antes de que los comandos de control funcionen.
El firmware cumple esto desde la corrección C-3; versiones anteriores arrancaban ARMADAS pese a lo que decía este documento.

**Secuencia de inicio:**
1. Armar el sistema: `M:SYS_ARM:0`
2. Establecer modo: `M:SYS_MODE:1` (AUTO) o `M:SYS_MODE:0` (MANUAL)
3. En modo AUTO: el sistema pasa a RUNNING automáticamente cuando recibe heartbeat
4. En modo MANUAL: el sistema necesita estar en estado RUNNING (requiere ARM + modo MANUAL)

**Los comandos de control (SET_SPEED, SET_STEER) solo funcionan cuando el sistema está ARMED y RUNNING.**

## Formato de Comandos

Todos los comandos deben seguir el formato:

```
CHANNEL:COMMAND:VALUE\n
```

- **CHANNEL**: Un solo carácter que identifica el canal
- **COMMAND**: Nombre del comando (mayúsculas)
- **VALUE**: Valor numérico entero
- **Terminación**: Cada comando debe terminar con `\n` (newline)

## Canales Disponibles

### `C` - CONTROL
Comandos de control del vehículo (velocidad, dirección)

### `E` - EMERGENCY  
Comandos de emergencia (frenado inmediato)

### `M` - MANAGEMENT
Comandos de gestión del sistema (armado, modo, etc.)

## Comandos por Canal

### Canal CONTROL (`C`)

#### `C:SET_SPEED:<valor>`
Establece la velocidad del motor de tracción.

- **Valor**: 0-255 (0 = detenido, 255 = máxima velocidad). Los valores fuera de rango se saturan: un negativo se trata como 0, un valor > 255 como 255.
- **TTL**: 200ms (el comando expira si no se renueva)
- **Ejemplo**: `C:SET_SPEED:120`
- **⚠️ UMBRAL DE ARRANQUE: 140** (medido en banco el 2026-08-04, con el vehículo cargado). Por debajo de ese valor el motor **no arranca desde parado**: el firmware acepta el comando y emite `EVENT:CMD_EXECUTED:SET_SPEED`, pero las ruedas no se mueven. Es una limitación física (fricción estática), no un fallo del firmware — y resulta engañosa, porque desde el lado del cliente todo parece haber funcionado.

  **Implicación para quien genere las consignas:** una conversión lineal desde otra escala de velocidad va a producir valores por debajo de 140 que el vehículo ignora en silencio. Hay que mapear el rango útil por encima del umbral, o dar un pulso de arranque y después bajar. La velocidad mínima para *mantener* el movimiento es menor que la de arranque; conviene medirla antes de calibrar el lazo de control.
- **⚠️ Comportamiento al expirar (FAIL-SAFE)**: si el comando expira o se pierde el enlace, **el vehículo frena**. **No hay velocidad por defecto.** Para mantener el vehículo en movimiento hay que reenviar `C:SET_SPEED` de forma periódica (ver §TTL: 10 Hz recomendado).

```python
# Ejemplo Python
ser.write(b"C:SET_SPEED:120\n")
```

#### `C:SET_STEER:<valor>`
Establece el ángulo de dirección del servo.

- **Valor**: 50-160 (50 = izquierda máxima, 105 = centro, 160 = derecha máxima). Fuera de rango se satura contra esos extremos.
- **TTL**: 200ms
- **Ejemplo**: `C:SET_STEER:105` (centro)

**⚠️ IMPORTANTE - Conversión de Grados a Valores de Servo:**

Si tu lane detector envía grados (ej: -45° a +45°), debes convertir así:

```python
# Fuente de verdad: include/hardware.h. El recorrido real es SIMÉTRICO.
SERVO_LEFT   = 50
SERVO_CENTER = 105
SERVO_RIGHT  = 160
SERVO_HALF_RANGE = 55   # == SERVO_CENTER - SERVO_LEFT == SERVO_RIGHT - SERVO_CENTER

def degrees_to_servo(degrees, max_degrees=45):
    """
    Convierte grados de lane detector a valor de servo.

    Args:
        degrees: Ángulo en grados (-max_degrees a +max_degrees).
                 Negativo = izquierda, positivo = derecha.
        max_degrees: Máximo ángulo permitido (default: 45°)

    Returns:
        Valor de servo (50-160)
    """
    normalized = max(-1.0, min(1.0, degrees / max_degrees))
    servo = SERVO_CENTER + normalized * SERVO_HALF_RANGE
    return int(round(max(SERVO_LEFT, min(SERVO_RIGHT, servo))))

# Ejemplos:
# degrees_to_servo(0)   -> 105 (centro)
# degrees_to_servo(-45) -> 50  (izquierda máxima)
# degrees_to_servo(45)  -> 160 (derecha máxima)
# degrees_to_servo(-20) -> 81  (izquierda suave)
```

> **⚠️ CAMBIO RESPECTO DE VERSIONES ANTERIORES DE ESTE DOCUMENTO.**
> Este documento declaraba `SERVO_RIGHT = 135`. El firmware siempre usó **160**.
> Si integraste contra 135, tus comandos eran válidos pero solo alcanzaban el 55 %
> del recorrido disponible hacia la derecha. Con la fórmula corregida **el auto
> girará más a la derecha que antes con los mismos grados de entrada**:
> hay que recalibrar el lazo de seguimiento de carril.
> Además, la función anterior era incorrecta incluso respecto de sus propias
> constantes (devolvía 63 para -45° y 148 para +45°, no 50 y 135).

**Ejemplo completo con lane detector:**

```python
# En lugar de solo imprimir "turn left" o "turn right":
lane_angle = -25  # grados desde tu detector

# Convertir y enviar:
servo_value = degrees_to_servo(lane_angle)
command = f"C:SET_STEER:{servo_value}\n"
ser.write(command.encode())
```

#### `C:SET_DIR:<0|1>`
Establece el **sentido de marcha**. No cambia la velocidad.

- **Valor**: `1` = adelante, `0` = atras
- **TTL**: no tiene. Es una bandera persistente, como en la pagina web del propio ESP32.
- **Ejemplo**: `C:SET_DIR:0` (marcha atras)
- **Respuesta**: `EVENT:CMD_RECEIVED:SET_DIR:FORWARD` / `:BACKWARD`

**Por que existe:** `C:SET_SPEED` satura los negativos a 0, asi que por el enlace
serie el vehiculo iba **siempre hacia adelante**. El sentido solo se podia cambiar
desde la pagina web del ESP32. Un dashboard con marcha atras mandaba velocidades
negativas y no pasaba nada.

**No hace falta reenviar `SET_SPEED` despues.** La inversion se aplica sola, aunque
la velocidad este latcheada, respetando el mismo tiempo muerto de 60 ms que evita
invertir el puente H con corriente (*plugging*). Reenviar la velocidad desde el
cliente era la solucion anterior y era peligrosa: mandaba primero la velocidad
**vieja**, asi que pedir "marcha atras suave" viniendo de "adelante a fondo" daba
un tiron a fondo hacia atras.

### Canal EMERGENCY (`E`)

#### `E:BRAKE_NOW:0`
Freno de emergencia. Detiene el motor y arranca un periodo de bloqueo de 5 s.

- **Latencia real**: hasta ~50 ms en el peor caso (polling de `LinkRxTask` 10 ms + ciclo de
  `MotorTask` 10 ms + bloqueo del ultrasonido hasta 30 ms). **No asumir <1 ms.** El E-STOP
  físico por GPIO sigue siendo el único mecanismo de parada verdaderamente inmediato.

- **Valor**: Siempre 0 (ignorado)
- **Ejemplo**: `E:BRAKE_NOW:0`

```python
ser.write(b"E:BRAKE_NOW:0\n")
```

#### `E:STOP:0`
Alias para freno de emergencia (mismo comportamiento que BRAKE_NOW).

### Canal MANAGEMENT (`M`)

#### `M:PING:0`
Latido del enlace. **No toca ningun mailbox y no cambia ningun estado**: su unico
efecto es alimentar el watchdog.

- **Valor**: siempre 0
- **Ejemplo**: `M:PING:0`

Es obligatorio con el firmware *latching*: como la velocidad se manda una sola vez
y despues no hay mas trafico, sin este ping el watchdog engancha `FAULT` al segundo.
Mandalo a **10 Hz** (ver §TTL).

#### `M:SYS_ARM:0`
**⚠️ REQUERIDO** - Arma el sistema (prepara para operación). Debe enviarse antes de cualquier comando de control.

- **Valor**: Siempre 0
- **TTL**: 5000ms
- **Ejemplo**: `M:SYS_ARM:0`
- **Nota**: Sin ARM, los comandos SET_SPEED y SET_STEER serán ignorados
- **Respuesta**: `EVENT:CMD_EXECUTED:SYS_ARM` (se emite siempre, incluso si ya estaba armado)
- **Rechazo**: `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE` si el E-STOP físico sigue accionado

#### `M:SYS_DISARM:0`
Desarma el sistema (modo seguro). Detiene el vehículo inmediatamente.

- **Valor**: Siempre 0
- **TTL**: 5000ms
- **Ejemplo**: `M:SYS_DISARM:0`
- **Efecto**: Detiene motor y centra dirección

#### `M:SYS_MODE:<valor>`
Establece el modo del sistema.

- **Valor**: `0` / `MANUAL`, o `1` / `AUTO` (se aceptan las cuatro formas, comparadas como texto)
- **TTL**: 5000ms
- **Ejemplo**: `M:SYS_MODE:1` (modo AUTO) o `M:SYS_MODE:AUTO`
- **Valor desconocido**: se responde `EVENT:CMD_REJECTED:SYS_MODE:BAD_VALUE` y **no se cambia el modo**
- **Nota**: En modo AUTO, el sistema pasa a RUNNING automáticamente cuando recibe heartbeat

## Ejemplos de Uso

### Ejemplo 1: Control Básico

```python
import serial

ser = serial.Serial('/dev/ttyUSB0', baudrate=115200, timeout=1)

# 1. Armar sistema (REQUERIDO)
ser.write(b"M:SYS_ARM:0\n")
time.sleep(0.1)

# 2. Establecer modo AUTO (opcional, pero recomendado)
ser.write(b"M:SYS_MODE:1\n")  # 1 = AUTO, 0 = MANUAL
time.sleep(0.1)

# 3. Ahora los comandos de control funcionarán
ser.write(b"C:SET_SPEED:150\n")
ser.write(b"C:SET_STEER:105\n")

# Frenar de emergencia (funciona siempre, incluso sin ARM)
ser.write(b"E:BRAKE_NOW:0\n")
```

### Ejemplo 2: Lane Following

```python
import serial
import time

ser = serial.Serial('/dev/ttyUSB0', baudrate=115200, timeout=1)

# INICIALIZACIÓN: Armar y configurar modo
ser.write(b"M:SYS_ARM:0\n")
time.sleep(0.1)
ser.write(b"M:SYS_MODE:1\n")  # Modo AUTO
time.sleep(0.1)

def degrees_to_servo(degrees, max_degrees=45):
    SERVO_CENTER = 105
    SERVO_HALF_RANGE = 55
    normalized = max(-1.0, min(1.0, degrees / max_degrees))
    return int(round(max(50, min(160, SERVO_CENTER + normalized * SERVO_HALF_RANGE))))

# Loop de control
while True:
    # Obtener ángulo del lane detector
    lane_angle = get_lane_angle()  # Tu función
    
    # Convertir y enviar comando de dirección
    servo_value = degrees_to_servo(lane_angle)
    command = f"C:SET_STEER:{servo_value}\n"
    ser.write(command.encode())
    
    # Mantener velocidad constante (heartbeat implícito en cada comando)
    ser.write(b"C:SET_SPEED:120\n")
    
    time.sleep(0.05)  # 20 Hz update rate
```

### Ejemplo 3: Control con Velocidad Variable

```python
def control_vehicle(speed, steering_degrees):
    """
    Función helper para controlar el vehículo.
    
    Args:
        speed: Velocidad 0-255
        steering_degrees: Ángulo de dirección en grados (-45 a +45)
    """
    # Validar velocidad
    speed = max(0, min(255, int(speed)))
    
    # Convertir dirección
    servo_value = degrees_to_servo(steering_degrees)
    
    # Enviar comandos
    ser.write(f"C:SET_SPEED:{speed}\n".encode())
    ser.write(f"C:SET_STEER:{servo_value}\n".encode())
```

## Consideraciones Importantes

### Time-to-Live (TTL) y watchdog
Los comandos tienen un tiempo de vida limitado:
- **CONTROL**: 200 ms - al expirar, **el vehículo frena** (fail-safe)
- **MANAGEMENT**: 5000 ms - los comandos de sistema duran más

Además hay un **watchdog de enlace** que lleva el sistema a `FAULT` si deja de llegar
cualquier comando válido:

| Modo | Timeout del watchdog |
|---|---|
| AUTO | 300 ms |
| MANUAL | 1000 ms |

La jerarquía que el firmware garantiza es `período de emisión < TTL < watchdog`:

```
período recomendado 100 ms (10 Hz)  <  TTL 200 ms  <  watchdog AUTO 300 ms
```

**Recomendación**: envía comandos de velocidad y dirección a **10-20 Hz**.
**Mínimo: 10 Hz.** (Versiones anteriores de este documento decían "mínimo 5 Hz";
era incorrecto: a 5 Hz el watchdog salta en cada ciclo.)

Solo un comando **reconocido y despachado** refresca el watchdog. Basura que
casualmente se parsee ya no lo mantiene vivo.

### Last-Writer-Wins
El sistema usa patrón "last-writer-wins". Si envías múltiples comandos rápidamente, solo el último es válido. No hay cola de comandos.

### Catálogo de eventos de salida

El ESP32 emite líneas `EVENT:...\n`. Estas son las que forman parte del protocolo
(cualquier otra línea, típicamente con prefijo `[NombreTarea]`, es depuración humana
y no debe parsearse):

| Evento | Cuándo |
|---|---|
| `EVENT:SYSTEM_READY` | Fin de `setup()`, todas las tareas creadas |
| `EVENT:STATE_CHANGED:<DISARMED\|ARMED\|RUNNING\|FAULT>` | Cambio de estado |
| `EVENT:MODE_CHANGED:<AUTO\|MANUAL>` | Cambio de modo |
| `EVENT:STATE_AUTO_TRANSITION:ARMED->RUNNING` | Transición automática |
| `EVENT:CMD_RECEIVED:<CMD>[:<valor>]` | Comando recibido y enrutado |
| `EVENT:CMD_EXECUTED:<CMD>[:<valor>]` | Comando aplicado |
| `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE` | ARM con E-STOP accionado |
| `EVENT:CMD_REJECTED:SYS_MODE:BAD_VALUE` | Valor de modo no reconocido |
| `EVENT:ESTOP_TRIGGERED:GPIO` | E-STOP físico accionado |
| `EVENT:ESTOP_RELEASED` | E-STOP físico liberado |
| `EVENT:WATCHDOG_TIMEOUT` | Pérdida de enlace |
| `EVENT:FATAL:TASK_CREATE_FAILED:<tarea>` | Fallo de arranque; el equipo reinicia |
| `EVENT:FATAL:MAILBOX_INIT_FAILED` | Fallo de arranque; el equipo reinicia |
| `EVENT:FATAL:TX_QUEUE_CREATE_FAILED` | Fallo de arranque; el equipo reinicia |

### Recuperación de FAULT

Se entra en `STATE_FAULT` por **watchdog** (pérdida de enlace) o por **E-STOP físico**.
FAULT bloquea todos los comandos de control. Para recuperarse:

1. **Eliminar la causa.** Si fue el E-STOP, liberarlo (llega `EVENT:ESTOP_RELEASED`).
   Si fue el watchdog, restablecer el enlace.
2. Enviar `M:SYS_ARM:0`.
   - Si el E-STOP sigue accionado se responde `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE`
     y el sistema **permanece** en FAULT.
   - Si la causa cesó, el sistema pasa a `ARMED` y responde `EVENT:CMD_EXECUTED:SYS_ARM`.
3. Reanudar el envío periódico de comandos de control.

`M:SYS_DISARM:0` seguido de `M:SYS_ARM:0` también funciona y es la secuencia más
conservadora. Tras un frenado de emergencia hay un **bloqueo de 5 s** durante el cual
el motor no responde aunque el sistema esté armado.

### Respuestas del ESP32
El ESP32 puede enviar mensajes de debug por serial. Puedes leerlos para debugging:

```python
if ser.in_waiting > 0:
    response = ser.readline().decode('utf-8', errors='ignore')
    print(f"ESP32: {response}")
```

### Manejo de Errores
- Si el comando no se parsea correctamente, el ESP32 imprime: `[LinkRxTask] Failed to parse message: ...`
- Verifica que el formato sea exacto: `CHANNEL:COMMAND:VALUE\n`
- Asegúrate de que el baud rate coincida (115200 para USB, 921600 para UART externo)
- **UART externo: el enlace usa ahora GPIO 17 (TX) y GPIO 16 (RX).** Antes estaba asignado a
  GPIO 9/10, que en el ESP32-WROOM-32 pertenecen al flash SPI interno y ni siquiera están
  expuestos en el conector del DevKit v1. Si tenías cableado a 9/10, hay que recablear.

## Checklist de Integración

- [ ] Configurar conexión serial (USB o UART externo)
- [ ] Implementar función de conversión grados → servo
- [ ] Enviar comandos periódicamente (10-20 Hz)
- [ ] Manejar freno de emergencia en caso de detección de obstáculos
- [ ] Implementar heartbeat/supervisión si es necesario
- [ ] Probar con el simulador UART antes de integrar con lane detector

## Simulador de Pruebas

Puedes probar tus comandos con el simulador incluido:

```bash
cd embedded
pip install -r test/python/requirements.txt
python3 test/python/test_uart_simulator.py /dev/ttyUSB0 --baud 115200
```

Luego prueba comandos como:
- `C:SET_SPEED:120`
- `C:SET_STEER:105`
- `E:BRAKE_NOW:0`

## Soporte

Si tienes dudas sobre el protocolo o encuentras problemas, consulta:
- `embedded/src/link_rx_task.cpp` - Implementación del parser
- `embedded/include/messages.h` - Definiciones de canales y comandos
- `embedded/include/hardware.h` - Valores de servo (SERVO_CENTER, etc.)

## Configuracion compartida con Brain-Aura

Los valores que los **dos** lados tienen que ver igual viven en un solo archivo,
`Brain-Aura/hardware.env`:

| Clave | `#define` del firmware |
|---|---|
| `BRAIN_SERVO_IZQUIERDA` | `SERVO_LEFT` |
| `BRAIN_SERVO_CENTRO` | `SERVO_CENTER` |
| `BRAIN_SERVO_DERECHA` | `SERVO_RIGHT` |
| `BRAIN_DUTY_MAX` | `MOTOR_SPEED_MAX` |
| `BRAIN_WATCHDOG_AUTO_MS` | `WATCHDOG_TIMEOUT_AUTO_MS` |
| `BRAIN_WATCHDOG_MANUAL_MS` | `WATCHDOG_TIMEOUT_MANUAL_MS` |
| `BRAIN_ULTRASONIC_UMBRAL_CM` | `ULTRASONIC_OBSTACLE_THRESHOLD_CM` |

`scripts/hardware_env.py` lo lee antes de compilar y los inyecta como `-D`; los
`#define` de `hardware.h` estan envueltos en `#ifndef`, asi que lo del archivo gana
y `hardware.h` queda solo como respaldo para compilar sin Brain-Aura al lado.

Cambiar cualquiera de esos valores **exige recompilar y reflashear**. La jerarquia
`periodo < TTL < watchdog` y el orden `SERVO_LEFT < SERVO_CENTER < SERVO_RIGHT` se
comprueban al compilar: si el archivo dice algo incoherente, el firmware no compila.

