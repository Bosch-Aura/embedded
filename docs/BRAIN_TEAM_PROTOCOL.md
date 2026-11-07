# Protocolo ESP32 ↔ Jetson

Cómo manejar el ESP32 (motor, dirección y ultrasonido) desde la Jetson o una PC.
Fuente de verdad: `src/link_rx_task.cpp` (comandos), `src/supervisor_task.cpp` (estados),
`src/control_task.cpp` (motor y servo), `src/ultrasonic_task.cpp` (obstáculos) e
`include/hardware.h` (valores numéricos).

## Conexión

- **USB**: 115200 baud. En la Jetson la placa aparece como `/dev/ttyCH341USB0`; en una PC, como `/dev/ttyUSB0`.
- **UART externo**: GPIO 17 (TX) / 16 (RX), 921600 baud.
- Los comandos se aceptan por los dos puertos, y **todos los `EVENT:` salen por los dos**.
- **Abrir el puerto reinicia la placa.** Esperá `EVENT:SYSTEM_READY` antes de mandar nada, y asumí que
  cada reconexión arranca de cero: `DISARMED` y nada latcheado.

## Formato

```
<CANAL>:<COMANDO>:<VALOR>\n        (\r también sirve como fin de línea)
```

`C` = control, `E` = emergencia, `M` = gestión. El valor es un entero; si el comando no lo usa, mandá `0`.
Se procesan todas las líneas completas que llegan (se revisa cada 10 ms). Una línea de más de 1023
caracteres se descarta entera.

## Estados

```
DISARMED --M:SYS_ARM--> RUNNING <--> STOPPED      (obstáculo o M:SYS_STOP)
    ^                      |            |
    +-----M:SYS_DISARM-----+------------+
cualquiera --E-STOP / watchdog--> FAULT --M:SYS_ARM--> RUNNING
```

| Estado | Motor | Servo |
|---|---|---|
| `DISARMED` | frenado, comandos ignorados | centrado |
| `RUNNING` | obedece | obedece |
| `STOPPED` por **obstáculo** | bloquea solo el avance; **la marcha atrás funciona** | obedece |
| `STOPPED` por **`M:SYS_STOP`** | frenado, comandos ignorados | congelado donde estaba |
| `FAULT` | frenado, comandos ignorados | centrado |

- La placa arranca en `DISARMED`. Si se compila con `-DBOOT_ARMED`, arranca en `RUNNING`.
- Cada vez que el auto frena por un estado (cualquiera menos `RUNNING`, o al intentar avanzar
  contra un obstáculo), **se borra la velocidad latcheada**. El auto nunca arranca solo: después
  hace falta un `C:SET_SPEED` nuevo.
- Un `C:SET_SPEED` ignorado por el estado se informa una vez por episodio con
  `EVENT:CMD_REJECTED:SET_SPEED:<DISARMED|FAULT|SYS_STOP|OBSTACLE>`. Un `C:SET_STEER` ignorado no se informa.

### Secuencia de inicio

1. Abrir el puerto y esperar `EVENT:SYSTEM_READY`.
2. Mandar `M:SYS_ARM:0` y esperar `EVENT:CMD_EXECUTED:SYS_ARM`.
3. Mantener vivo el enlace (ver [Watchdog](#watchdog)).

## Comandos

### Control (`C`)

| Comando | Efecto |
|---|---|
| `C:SET_SPEED:<0-255>` | PWM del motor. **Queda latcheado**: un solo comando sostiene la velocidad. `0` frena (IN3=IN4=LOW). Fuera de rango se satura (un negativo da 0). |
| `C:SET_DIR:<0\|1>` | Sentido de marcha: `1` adelante, `0` atrás. Queda latcheado. Si hay una velocidad latcheada, invierte solo. Entre la última vez que el motor anduvo en un sentido y la primera en el contrario pasan siempre al menos 60 ms sin PWM, **aunque en el medio haya frenado** (el motor sigue girando por inercia). |
| `C:SET_STEER:<50-160>` | Ángulo del servo: 50 = izquierda máxima, 105 = centro, 160 = derecha máxima. Fuera de rango se satura. Un ángulo pedido durante `SYS_STOP` se guarda y se aplica al reanudar. |

**Umbral de arranque: 130.** Es el PWM mínimo para que el motor se mueva desde parado (medido en
banco el 2026-11-06 con `scripts/prueba_freno.py`). Con menos, por ejemplo 80, el motor zumba pero no
gira (fricción estática): el firmware lo aplica igual y emite `CMD_EXECUTED:SET_SPEED`, así que el fallo
es silencioso. Hay que mapear el rango útil por encima de ese valor. En el piso, con el auto cargado,
puede hacer falta algo más.

Conversión de grados a valores del servo (el recorrido es simétrico, ±55 alrededor de 105):

```python
def degrees_to_servo(deg, max_deg=45):
    n = max(-1.0, min(1.0, deg / max_deg))
    return int(round(105 + n * 55))   # -45 -> 50, 0 -> 105, +45 -> 160
```

### Emergencia (`E`)

| Comando | Efecto |
|---|---|
| `E:BRAKE_NOW:0` (alias `E:STOP:0`) | Frena en el acto y borra la velocidad latcheada. **Es de un solo uso**: el próximo `C:SET_SPEED` vuelve a mover el auto (ya no hay bloqueo de 5 s). Comparte canal con `SET_SPEED`, así que **decide el orden de llegada**: `SET_SPEED` y después `BRAKE_NOW` termina frenado; al revés, termina andando. Funciona en cualquier estado y no cambia el estado. Para una parada que se mantenga, usá `M:SYS_STOP`. |

### Gestión (`M`)

| Comando | Efecto |
|---|---|
| `M:PING:0` | Solo alimenta el watchdog. No genera respuesta. |
| `M:SYS_ARM:0` | `DISARMED`/`FAULT` → `RUNNING` (o `STOPPED`, si hay una parada activa). Siempre confirma con `CMD_EXECUTED:SYS_ARM`, aunque ya estuviera armado. Con el E-STOP accionado responde `CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE`. Se aplica en ≤ 20 ms. `SYS_ARM` y `SYS_DISARM` se procesan **en orden de llegada**, aunque lleguen en el mismo ciclo. |
| `M:SYS_DISARM:0` | → `DISARMED`: frena y centra el servo. La velocidad se borra en el acto, sin esperar al supervisor. Siempre confirma con `CMD_EXECUTED:SYS_DISARM`. |
| `M:SYS_STOP:0` | Parada enganchada: `STOPPED`, motor frenado (en los dos sentidos) y servo congelado. **Solo la suelta `M:SYS_RESUME:0`**: sobrevive a `DISARM`, `FAULT` y a volver a armar. Un reinicio de la placa la borra. Siempre confirma con `CMD_EXECUTED:SYS_STOP`. |
| `M:SYS_RESUME:0` | Suelta el `SYS_STOP`. No reanuda nada solo: hace falta un `C:SET_SPEED` nuevo. Si hay un obstáculo, el auto sigue en `STOPPED`. Siempre confirma con `CMD_EXECUTED:SYS_RESUME`. |
| `M:OBSTACLE_RESET:0` | Suelta la parada por obstáculo (ver [Obstáculos](#obstáculos)). Si el obstáculo sigue ahí, en ~150 ms se vuelve a bloquear. |

Cualquier otro comando (incluido el viejo `M:LIGHTS_*`) se ignora, queda registrado como
`[LinkRx] unknown command` y **no alimenta el watchdog**.

## Watchdog

Si durante más de **300 ms** (`WATCHDOG_TIMEOUT_MS`) no llega ningún comando reconocido, el auto
entra en `FAULT`: frena, centra el servo y borra la velocidad. Esto se controla en `RUNNING` y en
`STOPPED`.

- Mandá algún comando reconocido a **10–20 Hz**. Puede ser `M:PING:0` o los propios comandos de control.
- Contando el período de revisión y la separación entre pings, el corte real ocurre a unos **350–370 ms**
  del último comando.
- Arranca a contar con el primer comando. `SYS_ARM` lo reinicia. Funciona igual cuando el contador de ticks
  de 32 bits da la vuelta (~49,7 días).
- `FAULT` **no se borra solo**: hay que eliminar la causa y mandar `M:SYS_ARM:0`. Después hace falta un
  `C:SET_SPEED` nuevo. El servo queda centrado, así que reenviá también el ángulo.

## Obstáculos

El ultrasonido (HC-SR04) se lee cada ~60–80 ms. El eco se mide con una interrupción en los dos flancos, y
la tarea duerme mientras espera (hasta 30 ms), sin ocupar el core del control. Con `-DULTRASONIC_USE_PULSEIN`
se vuelve a `pulseIn()`. **Solo cuentan las lecturas válidas** (de 2 a 400 cm). Una
lectura sin eco o fuera de rango no prueba nada (puede ser espacio abierto, un objeto muy cerca, una
superficie blanda o un sensor muerto), así que **no frena, no despeja y no corta una cuenta**.

- **Frena** con 3 lecturas válidas seguidas **< 30 cm** → `EVENT:OBSTACLE_DETECTED:<cm>` → `STOPPED`.
- **Despeja** con 3 lecturas válidas seguidas **≥ 35 cm** → `EVENT:OBSTACLE_CLEARED` → `RUNNING`.
- "Seguidas" se cuenta entre lecturas válidas: una válida del tipo contrario reinicia la cuenta. Además,
  **la evidencia vence**: si pasa más de 1 s entre dos lecturas válidas de una cuenta, la cuenta empieza de
  nuevo. Así, tres lecturas falsas espaciadas no suman una parada.
- La franja de 30 a 34 cm es histéresis, para que un objeto justo en el límite no haga oscilar el freno.
- **Solo se bloquea el avance** (el sensor mira hacia adelante; si se monta atrás, se compila con
  `-DULTRASONIC_FACING_FORWARD=0`). Con un obstáculo delante se puede retroceder, y el servo obedece. Si
  después se pide avanzar, el auto frena y se borra la velocidad.
- Al despejarse el auto **no reanuda solo**: la Jetson tiene que reenviar la velocidad. El dashboard de
  AURA lo hace automáticamente.
- **Si se saca el obstáculo y no queda nada dentro del alcance, no llegan lecturas lejanas válidas y el
  auto queda detenido.** Para destrabarlo está `M:OBSTACLE_RESET:0`.
- Latencia medida en el simulador, desde la primera lectura cercana hasta el PWM en 0: ~120 ms con un
  sensor limpio. Con pérdida de ecos crece (porque hacen falta 3 lecturas válidas): ~150 ms con 50% de
  pérdida, ~315 ms con 75% y ~1 s con 90%.

Salud del sensor (eventos **informativos**, nunca frenan):
- `EVENT:ULTRASONIC_SILENT`: 2 s sin ninguna lectura válida. Puede ser espacio abierto o un sensor
  desconectado; el firmware no puede saber cuál.
- `EVENT:ULTRASONIC_OK`: volvió a llegar una lectura válida.

En el banco, cualquier cosa a menos de 30 cm (una pared, una caja, una mano) frena el auto.

## Eventos

Toda línea que empieza con `EVENT:` es protocolo. Lo demás (`[Control] ...`, `[Supervisor] ...`, etc.) es
depuración: sale solo por USB y no hay que parsearlo. Cada línea llega entera (se escribe de una sola vez
bajo un mutex) y se trunca a ~157 caracteres.

| Evento | Cuándo |
|---|---|
| `EVENT:SYSTEM_READY` | Terminó el arranque |
| `EVENT:STATE_CHANGED:<DISARMED\|RUNNING\|STOPPED\|FAULT>` | Cambio de estado (al arrancar se informa el inicial) |
| `EVENT:CMD_EXECUTED:SET_SPEED:<pwm>` | Cambió la velocidad latcheada (valor ya saturado; un freno produce `SET_SPEED:0`) |
| `EVENT:CMD_REJECTED:SET_SPEED:<DISARMED\|FAULT\|SYS_STOP\|OBSTACLE>` | Un `SET_SPEED` > 0 ignorado por el estado (una vez por episodio) |
| `EVENT:CMD_EXECUTED:SET_STEER:<ángulo>` | El servo se movió (incluye el centrado, como `SET_STEER:105`) |
| `EVENT:CMD_EXECUTED:BRAKE_NOW` | Se aplicó `E:BRAKE_NOW` / `E:STOP` |
| `EVENT:CMD_EXECUTED:<SYS_ARM\|SYS_DISARM\|SYS_STOP\|SYS_RESUME\|OBSTACLE_RESET>` | Se aplicó el comando. Siempre se confirma, aunque no haya cambiado nada |
| `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE` | Intento de armar con el E-STOP accionado |
| `EVENT:ESTOP_TRIGGERED:GPIO` / `EVENT:ESTOP_RELEASED` | E-STOP físico (GPIO 4, activo en bajo) |
| `EVENT:WATCHDOG_TIMEOUT` | Pérdida de enlace → `FAULT` |
| `EVENT:OBSTACLE_DETECTED:<cm>` / `EVENT:OBSTACLE_CLEARED` | Ver [Obstáculos](#obstáculos) |
| `EVENT:ULTRASONIC_SILENT` / `EVENT:ULTRASONIC_OK` | Salud del sensor (informativo) |
| `EVENT:FATAL:TASK_CREATE_FAILED:<tarea>` | Falla de arranque; la placa se reinicia |

Ya no existen `CMD_RECEIVED:*`, `STATE_AUTO_TRANSITION:*`, `SET_STEER_CENTER`, `EMERGENCY_BRAKE`,
`LIGHTS_*` ni el estado `ARMED`.

## Ejemplo mínimo

```python
import serial, threading, time

ser = serial.Serial("/dev/ttyCH341USB0", 115200, timeout=1)   # /dev/ttyUSB0 en una PC
while b"EVENT:SYSTEM_READY" not in ser.readline():            # abrir el puerto reinicia la placa
    pass

def heartbeat():
    while True:
        ser.write(b"M:PING:0\n")
        time.sleep(0.05)                                      # 20 Hz < watchdog de 300 ms
threading.Thread(target=heartbeat, daemon=True).start()

ser.write(b"M:SYS_ARM:0\n")
ser.write(b"C:SET_DIR:1\n")
ser.write(b"C:SET_SPEED:180\n")     # velocidad crucero: queda latcheada
ser.write(b"C:SET_STEER:90\n")
time.sleep(2)
ser.write(b"M:SYS_STOP:0\n")        # stop enganchado
time.sleep(1)
ser.write(b"M:SYS_RESUME:0\n")      # start: suelta el stop...
ser.write(b"C:SET_SPEED:180\n")     # ...y hay que volver a mandar la velocidad
```

## Clientes conocidos

- **AURA-2026** (`brain/command_sender.py`, `brain/dashboard/dashboard_server.py`): manda `M:PING` a 20 Hz y arma al
  recibir `SYSTEM_READY`. Al recibir un `OBSTACLE_CLEARED` que termina un bloqueo, reenvía la última velocidad
  (no lo hace después de un `OBSTACLE_RESET`). Al armar, reenvía el ángulo. Con `FAULT`, `DISARMED` o un
  reinicio de la placa olvida la última velocidad y cancela lo que tengan programado las estrategias; lo mismo
  hacen el freno y el stop del dashboard. Tiene un botón para `OBSTACLE_RESET`. No vuelve a armar solo después de
  un `FAULT`.
- **`bfmc_hardware_bridge`** (ROS2): arma al conectar y manda `SET_STEER` + `SET_SPEED` a 20 Hz.

## Configuración compartida con Brain-Aura

Los valores que los dos lados tienen que ver iguales (topes del servo, watchdog, pines) se pueden definir en
`Brain-Aura/hardware.env`:

```
# firmware: SERVO_LEFT
BRAIN_SERVO_IZQUIERDA=50
```

`scripts/hardware_env.py` lee ese archivo antes de compilar y emite `-DSERVO_LEFT=50`. Los `#define` de
`include/hardware.h` están envueltos en `#ifndef`, así que gana lo del archivo; si alguno no está envuelto,
el script corta la compilación. Además, se comprueba al compilar que `SERVO_LEFT < SERVO_CENTER <
SERVO_RIGHT` y que el watchdog sea mayor que `LINK_EXPECTED_PERIOD_MS`. Cambiar cualquiera de estos valores
exige recompilar y volver a flashear.
