# Protocolo de Comunicación ESP32 - Guía para el Equipo Brain

Este documento describe cómo enviar comandos desde el sistema Brain (Jetson/PC) hacia el ESP32 que controla los motores, dirección y luces del vehículo.

## ⚠️ IMPORTANTE: Sistema de Seguridad (ARM/DISARM)

**El estado de arranque depende de cómo se compile el firmware:**

| Compilación | Estado al arrancar | Qué pasa |
|---|---|---|
| **Con `-DBOOT_ARMED`** (agregándolo a `build_flags`) | `ARMED`, modo `MANUAL` | En MANUAL pasa solo a `RUNNING` (`EVENT:STATE_AUTO_TRANSITION:ARMED->RUNNING`): el **primer** `C:SET_SPEED` que llegue se obedece, sin handshake. |
| **Sin `-DBOOT_ARMED`** (es la que está hoy en `platformio.ini`) | `DISARMED`, modo `MANUAL` | Ningún comando de control se ejecuta hasta recibir `M:SYS_ARM:0`. Es la opción más segura (corrección C-3). |

Con el `platformio.ini` actual la placa **arranca `DISARMED`**: el cliente tiene que mandar
`M:SYS_ARM:0` para que obedezca. Igual, no dependas de la compilación: **mandá siempre
`M:SYS_ARM:0` al conectar**. Es idempotente (si ya estaba armado responde igual
`EVENT:CMD_EXECUTED:SYS_ARM`) y funciona con las dos compilaciones.

Con `-DBOOT_ARMED`, el watchdog recién empieza a contar con el **primer** comando válido: una placa
que arranca armada y no recibe nada se queda en `RUNNING` sin entrar en `FAULT` (tampoco se mueve,
porque no hay ninguna velocidad latcheada).

**Secuencia de inicio recomendada:**
1. Abrir el puerto y **esperar `EVENT:SYSTEM_READY`** (en la Jetson abrir el puerto reinicia la placa, ver §Conexión).
2. Establecer modo: `M:SYS_MODE:1` (AUTO) o `M:SYS_MODE:0` (MANUAL). Esperar `EVENT:CMD_EXECUTED:SYS_MODE:<modo>`
   (o, si ya estaba en ese modo, solo llega `EVENT:CMD_RECEIVED:SYS_MODE:<modo>`).
3. Armar: `M:SYS_ARM:0`. Esperar `EVENT:CMD_EXECUTED:SYS_ARM`.
4. Mantener vivo el enlace (`M:PING:0` o comandos de control) más seguido que el watchdog, ver §TTL y watchdog.

No mandes dos comandos `M:` pegados: el supervisor tiene **un solo lugar** para comandos de gestión
y lo revisa cada 20 ms, así que el segundo puede pisar al primero. Separalos al menos ~50 ms o,
mejor, esperá el `EVENT:` de confirmación del anterior.

**Cuándo se obedecen los comandos de control (SET_SPEED, SET_STEER):**
- En modo **AUTO**: solo en estado `RUNNING`. Se pasa de `ARMED` a `RUNNING` solo, en cuanto hay un heartbeat reciente (< 300 ms); como `SYS_ARM` cuenta como heartbeat, normalmente ocurre al armar.
- En modo **MANUAL**: en `ARMED` o `RUNNING` (y `ARMED` pasa solo a `RUNNING`).
- En `DISARMED` o `FAULT` se ignoran **en silencio**: no hay `EVENT:` de rechazo, solo una línea de depuración `[MotorTask] SET_SPEED ignored - system DISARMED` / `[SteerTask] SET_STEER ignored ...`.

## Formato de Comandos

Todos los comandos deben seguir el formato:

```
CHANNEL:COMMAND:VALUE\n
```

- **CHANNEL**: Un solo carácter que identifica el canal
- **COMMAND**: Nombre del comando (mayúsculas)
- **VALUE**: Valor numérico entero (excepción: `M:SYS_MODE` acepta también `AUTO`/`MANUAL`)
- **Terminación**: Cada comando debe terminar con `\n` (newline); `\r` también sirve como terminador

`LinkRxTask` procesa **como máximo una línea cada 10 ms** (≈100 líneas/s): en cada vuelta lee una
línea del USB y, solo si ahí no había ninguna completa, una del UART externo. Si mandás más rápido, las líneas no se pierden pero se acumulan en el buffer y llegan con retraso. A 20 Hz
con dos comandos por ciclo (40 líneas/s) hay margen de sobra.

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
- **TTL**: 200 ms en el mailbox. **Al vencer, el motor NO frena** (ver *latching* más abajo).
- **Ejemplo**: `C:SET_SPEED:120`
- **Respuesta**: cada `C:SET_SPEED` recibido produce `EVENT:CMD_RECEIVED:SET_SPEED:<v>` (con el valor tal
  como llegó, antes de saturar), aunque después se ignore. Sirve como confirmación de vida del lado
  del cliente. `EVENT:CMD_EXECUTED:SET_SPEED:<v>` sale solo cuando la velocidad aplicada **cambia**.
- **⚠️ UMBRAL DE ARRANQUE: 140** (medido en banco el 2026-08-04, con el vehículo cargado). Por debajo de ese valor el motor **no arranca desde parado**: el firmware acepta el comando y emite `EVENT:CMD_EXECUTED:SET_SPEED`, pero las ruedas no se mueven. Es una limitación física (fricción estática), no un fallo del firmware — y resulta engañosa, porque desde el lado del cliente todo parece haber funcionado.

  **Implicación para quien genere las consignas:** una conversión lineal desde otra escala de velocidad va a producir valores por debajo de 140 que el vehículo ignora en silencio. Hay que mapear el rango útil por encima del umbral, o dar un pulso de arranque y después bajar. La velocidad mínima para *mantener* el movimiento es menor que la de arranque; conviene medirla antes de calibrar el lazo de control.
- **⚠️ Control *latching***: **un solo `C:SET_SPEED` sostiene la velocidad.** Cuando el comando vence
  su TTL de 200 ms el motor **sigue con la última velocidad**; no hace falta reenviarla. Esto
  incluye `C:SET_SPEED:0`: queda latcheado y el auto se queda parado hasta un `SET_SPEED` nuevo.
  (Versiones anteriores de este documento decían que al expirar el vehículo frenaba y que había
  que reenviar `SET_SPEED` a 10 Hz: ya no es así.)

  **Lo que protege ante la pérdida del enlace es el watchdog, no el TTL.** Si deja de llegar
  *cualquier* comando válido durante más de 300 ms (AUTO) o 1000 ms (MANUAL), el sistema entra en
  `FAULT`, frena y borra la velocidad latcheada. Por eso, si mandás la velocidad una sola vez,
  tenés que mantener el enlace con `M:PING:0` (ver §TTL y watchdog).

  La velocidad latcheada también se borra con `E:BRAKE_NOW`, `M:SYS_DISARM`, E-STOP y ultrasonido:
  después de cualquiera de ellos el auto queda parado hasta recibir un `SET_SPEED` nuevo. Nunca
  vuelve a arrancar solo.

```python
# Ejemplo Python
ser.write(b"C:SET_SPEED:120\n")
```

#### `C:SET_STEER:<valor>`
Establece el ángulo de dirección del servo.

- **Valor**: 50-160 (50 = izquierda máxima, 105 = centro, 160 = derecha máxima). Fuera de rango se satura contra esos extremos.
- **TTL**: 200 ms en el mailbox. Igual que la velocidad, **se mantiene**: al vencer, el servo se queda
  en el último ángulo. Solo se recentra con `M:SYS_DISARM`, watchdog o E-STOP
  (`EVENT:CMD_EXECUTED:SET_STEER_CENTER`). Ni `E:BRAKE_NOW` ni el ultrasonido centran la dirección.
- **Ejemplo**: `C:SET_STEER:105` (centro)
- **Respuesta**: `EVENT:CMD_RECEIVED:SET_STEER:<v>` por cada comando; `EVENT:CMD_EXECUTED:SET_STEER:<v>`
  solo cuando el ángulo aplicado cambia.

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

Si el motor esta **frenado** (despues de `E:BRAKE_NOW`, `SYS_DISARM` o un `FAULT`) no hay nada
latcheado que invertir: el sentido pedido se aplica con el proximo `C:SET_SPEED`.

### Canal EMERGENCY (`E`)

#### `E:BRAKE_NOW:0`
Freno de emergencia. Detiene el motor y arranca un periodo de bloqueo de 5 s.

- **Latencia real**: hasta ~50 ms en el peor caso (polling de `LinkRxTask` 10 ms + ciclo de
  `MotorTask` 10 ms + bloqueo del ultrasonido hasta 30 ms). **No asumir <1 ms.** El E-STOP
  físico por GPIO sigue siendo el único mecanismo de parada verdaderamente inmediato.

- **Valor**: Siempre 0 (ignorado)
- **Ejemplo**: `E:BRAKE_NOW:0`
- **Funciona en cualquier estado**, incluso `DISARMED`. **No cambia el estado** (si estaba en `RUNNING`, sigue en `RUNNING`).
- **Respuesta**: `EVENT:CMD_RECEIVED:BRAKE_NOW` y, al aplicarse, `EVENT:CMD_EXECUTED:EMERGENCY_BRAKE`.
- **Después del freno**: durante 5 s la tracción queda bloqueada y todo `C:SET_SPEED` se ignora
  (`[MotorTask] Speed command ignored (in cooldown)`). Pasados los 5 s el auto **sigue parado**: la
  velocidad latcheada se borró, así que hace falta un `C:SET_SPEED` nuevo para moverse. La
  dirección no se toca.

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
y despues no hay mas trafico, sin este ping el watchdog engancha `FAULT` a los 300 ms
en AUTO (1000 ms en MANUAL). Mandalo a **10 Hz** (ver §TTL).

- **Respuesta**: ninguna. `PING` no produce ningun `EVENT:` (para confirmar que el enlace
  esta vivo del lado cliente, usa el `EVENT:CMD_RECEIVED:SET_SPEED:<v>` de cada `SET_SPEED`).

#### `M:SYS_ARM:0`
Arma el sistema. Con la compilación actual la placa arranca desarmada, así que hay que
**mandarlo** al conectar y es el único camino para salir de `FAULT` (ver §Sistema de Seguridad).

- **Valor**: Siempre 0
- **TTL**: 5000 ms en el mailbox, pero se ejecuta **una sola vez** por comando recibido
- **Ejemplo**: `M:SYS_ARM:0`
- **Nota**: En `DISARMED` o `FAULT`, los comandos SET_SPEED y SET_STEER se ignoran. Armar cuenta como heartbeat.
- **Respuesta**: `EVENT:CMD_RECEIVED:SYS_ARM` y `EVENT:CMD_EXECUTED:SYS_ARM` (este último se emite siempre, incluso si ya estaba armado). Si hubo cambio de estado, además `EVENT:STATE_CHANGED:ARMED` (y en seguida `RUNNING`).
- **Rechazo**: `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE` si el E-STOP físico sigue accionado

#### `M:SYS_DISARM:0`
Desarma el sistema (modo seguro). Detiene el vehículo inmediatamente.

- **Valor**: Siempre 0
- **TTL**: 5000 ms en el mailbox, se ejecuta una sola vez
- **Ejemplo**: `M:SYS_DISARM:0`
- **Efecto**: freno de emergencia (con el mismo bloqueo de 5 s que `E:BRAKE_NOW`), borra la velocidad latcheada y centra la dirección
- **Respuesta**: `EVENT:CMD_RECEIVED:SYS_DISARM`, `EVENT:CMD_EXECUTED:SYS_DISARM`, `EVENT:STATE_CHANGED:DISARMED`, `EVENT:CMD_EXECUTED:EMERGENCY_BRAKE` y, si la dirección no estaba centrada, `EVENT:CMD_EXECUTED:SET_STEER_CENTER`. Si ya estaba `DISARMED`, solo llega el `CMD_RECEIVED`.

#### `M:SYS_MODE:<valor>`
Establece el modo del sistema.

- **Valor**: `0` / `MANUAL`, o `1` / `AUTO` (se aceptan las cuatro formas, comparadas como texto)
- **TTL**: 5000 ms en el mailbox, se ejecuta una sola vez
- **Ejemplo**: `M:SYS_MODE:1` (modo AUTO) o `M:SYS_MODE:AUTO`
- **Respuesta**: `EVENT:CMD_RECEIVED:SYS_MODE:<AUTO|MANUAL>`, `EVENT:CMD_EXECUTED:SYS_MODE:<modo>` y `EVENT:MODE_CHANGED:<modo>`.
  **Si el modo ya era ese**, el supervisor no hace nada y **solo** llega el `EVENT:CMD_RECEIVED:SYS_MODE:<modo>`: no esperes un `CMD_EXECUTED` que no va a venir.
- **Valor desconocido**: se responde `EVENT:CMD_REJECTED:SYS_MODE:BAD_VALUE` y **no se cambia el modo**
- **Nota**: al pasar a AUTO se reinicia el heartbeat: el watchdog deja de contar hasta el próximo comando válido y, si el sistema estaba en `ARMED`, pasa a `RUNNING` recién con ese comando.

#### `M:LIGHTS_ON:0`, `M:LIGHTS_OFF:0`, `M:LIGHTS_AUTO:0`
Luces delanteras: encendidas, apagadas o automáticas (según el LDR).

- **Respuesta**: `EVENT:CMD_RECEIVED:LIGHTS_<X>` y, si el modo de luces cambió, `EVENT:CMD_EXECUTED:LIGHTS_<X>`
- **Latencia**: `LightsTask` corre a 1 Hz, así que el cambio puede tardar hasta ~1 s

## Ejemplos de Uso

> **Puerto:** los ejemplos usan `PORT`. En la Jetson, con el driver de WCH, la placa aparece como
> `/dev/ttyCH341USB0` (no `/dev/ttyUSB0`); en una PC con el driver estándar suele ser
> `/dev/ttyUSB0`. Ver §Conexión.

### Ejemplo 1: Control Básico

```python
import serial
import time

PORT = "/dev/ttyCH341USB0"   # Jetson; /dev/ttyUSB0 en una PC
ser = serial.Serial(PORT, baudrate=115200, timeout=1)

# 0. Abrir el puerto reinicia la placa: esperar a que termine de arrancar
#    (si en tu equipo abrir el puerto no la reinicia, este bucle no termina: poné un límite)
while b"EVENT:SYSTEM_READY" not in ser.readline():
    pass

# 1. Establecer modo AUTO (opcional, pero recomendado)
ser.write(b"M:SYS_MODE:1\n")  # 1 = AUTO, 0 = MANUAL
time.sleep(0.1)               # no mandar dos M: pegados

# 2. Armar sistema (obligatorio: la placa arranca desarmada)
ser.write(b"M:SYS_ARM:0\n")
time.sleep(0.1)

# 3. Ahora los comandos de control funcionarán. Un solo SET_SPEED alcanza (latching)
ser.write(b"C:SET_SPEED:150\n")
ser.write(b"C:SET_STEER:105\n")

# 4. ...pero el enlace hay que mantenerlo vivo: sin tráfico, FAULT a los 300 ms en AUTO
for _ in range(20):           # ~2 s andando
    ser.write(b"M:PING:0\n")
    time.sleep(0.1)           # 10 Hz

# Frenar de emergencia (funciona siempre, incluso sin ARM)
ser.write(b"E:BRAKE_NOW:0\n")
```

### Ejemplo 2: Lane Following

```python
import serial
import time

ser = serial.Serial("/dev/ttyCH341USB0", baudrate=115200, timeout=1)  # /dev/ttyUSB0 en una PC
while b"EVENT:SYSTEM_READY" not in ser.readline():  # abrir el puerto reinicia la placa
    pass

# INICIALIZACIÓN: configurar modo y armar
ser.write(b"M:SYS_MODE:1\n")  # Modo AUTO
time.sleep(0.1)
ser.write(b"M:SYS_ARM:0\n")
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
    
    # Reenviar la velocidad no hace falta (latching), pero tampoco molesta:
    # cada comando válido alimenta el watchdog, así que acá no hace falta PING
    ser.write(b"C:SET_SPEED:120\n")
    
    time.sleep(0.05)  # 20 Hz update rate
```

Este patrón (SET_STEER + SET_SPEED a 20 Hz) es el que usa el nodo ROS2 `bfmc_hardware_bridge`.
El patrón alternativo, apoyarse en el *latching* y mantener el enlace con `M:PING:0` a 10 Hz, es
el que usa Brain-Aura. Los dos son válidos: lo único obligatorio es que el hueco entre dos comandos válidos
nunca supere el timeout del watchdog.

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
Los comandos tienen un tiempo de vida limitado en su mailbox:
- **CONTROL**: 200 ms. Al expirar **no pasa nada visible**: velocidad, dirección y sentido quedan
  latcheados en el último valor. (Antes decía "al expirar, el vehículo frena": ya no es así.)
- **MANAGEMENT**: 5000 ms. Igual se ejecutan una sola vez por comando recibido.

Lo que detiene el vehículo si se cae el enlace es el **watchdog de enlace**, que lleva el sistema
a `FAULT` (frena, centra la dirección y borra la velocidad latcheada) si deja de llegar
cualquier comando válido:

| Modo | Timeout del watchdog |
|---|---|
| AUTO | 300 ms |
| MANUAL | 1000 ms |

La jerarquía que el firmware garantiza es `período de emisión < TTL < watchdog`:

```
período recomendado 100 ms (10 Hz)  <  TTL 200 ms  <  watchdog AUTO 300 ms
```

**Recomendación**: mandá **algún comando válido a 10-20 Hz**. Puede ser `M:PING:0` (si la
velocidad se manda solo cuando cambia) o los propios `SET_SPEED`/`SET_STEER`.
**Mínimo: 10 Hz.** (El watchdog de AUTO es de 300 ms; a 5 Hz el hueco entre comandos es de
200 ms y cualquier demora del lado del cliente lo hace saltar.)

Solo un comando **reconocido y despachado** refresca el watchdog: `E:BRAKE_NOW`/`E:STOP`, `C:SET_SPEED`,
`C:SET_STEER`, `C:SET_DIR`, `M:PING`, `M:SYS_ARM`, `M:SYS_DISARM`, `M:SYS_MODE` (con valor válido) y
`M:LIGHTS_*`. Basura que casualmente se parsee no lo mantiene vivo.

> **Watchdog falso al armar (corregido en b4acb21).** Firmwares anteriores a ese commit podían
> entrar en `FAULT` justo después de `M:SYS_ARM:0`, con comandos llegando cada 50 ms, por una resta
> sin signo que daba la vuelta. Se reconocía por la línea
> `[SupervisorTask] Watchdog timeout! Heartbeat age: 4294967271 ms` (un número cercano a 2³²). Si
> la ves, la placa tiene un firmware viejo: hay que reflashear.

### Last-Writer-Wins
El sistema usa patrón "last-writer-wins". Si envías múltiples comandos rápidamente, solo el último es válido. No hay cola de comandos.

Cada mailbox tiene **un solo lugar**: motor, dirección, luces y supervisor por separado. Para los
comandos de control esto no molesta (el último es el que vale), pero para los de gestión sí:
`SupervisorTask` revisa su mailbox cada 20 ms, así que dos comandos `M:` mandados con menos de
~20-50 ms de separación pueden pisarse y el primero no ejecutarse nunca. Espacialos o esperá el
`EVENT:` de confirmación de cada uno.

### Catálogo de eventos de salida

El ESP32 emite líneas `EVENT:...\r\n`. Estas son las que forman parte del protocolo
(cualquier otra línea, típicamente con prefijo `[NombreTarea]`, es depuración humana
y no debe parsearse):

| Evento | Cuándo |
|---|---|
| `EVENT:SYSTEM_READY` | Fin de `setup()`, todas las tareas creadas |
| `EVENT:STATE_CHANGED:<DISARMED\|ARMED\|RUNNING\|FAULT>` | Cambio de estado. Al arrancar se informa el estado inicial (puede llegar dos veces y antes o después de `SYSTEM_READY`) |
| `EVENT:MODE_CHANGED:<AUTO\|MANUAL>` | Cambio de modo. Al arrancar, igual que el anterior |
| `EVENT:STATE_AUTO_TRANSITION:ARMED->RUNNING` | Transición automática |
| `EVENT:CMD_RECEIVED:SET_SPEED:<v>` | Cada `C:SET_SPEED` recibido (valor tal como llegó), aunque después se ignore |
| `EVENT:CMD_RECEIVED:SET_STEER:<v>` | Cada `C:SET_STEER` recibido |
| `EVENT:CMD_RECEIVED:SET_DIR:<FORWARD\|BACKWARD>` | Cada `C:SET_DIR` recibido |
| `EVENT:CMD_RECEIVED:BRAKE_NOW` | `E:BRAKE_NOW` o `E:STOP` recibido |
| `EVENT:CMD_RECEIVED:<SYS_ARM\|SYS_DISARM>` | Comando de armado recibido |
| `EVENT:CMD_RECEIVED:SYS_MODE:<AUTO\|MANUAL>` | `M:SYS_MODE` con valor válido (se emite aunque el modo no cambie) |
| `EVENT:CMD_RECEIVED:<LIGHTS_ON\|LIGHTS_OFF\|LIGHTS_AUTO>` | Comando de luces recibido |
| `EVENT:CMD_EXECUTED:SET_SPEED:<v>` | La velocidad aplicada cambió (valor ya saturado a 0-255) |
| `EVENT:CMD_EXECUTED:SET_STEER:<v>` | El ángulo aplicado cambió (valor ya saturado a 50-160) |
| `EVENT:CMD_EXECUTED:SET_STEER_CENTER` | Dirección centrada por `SYS_DISARM`, watchdog o E-STOP |
| `EVENT:CMD_EXECUTED:EMERGENCY_BRAKE` | Se aplicó un freno de emergencia, venga de donde venga: `E:BRAKE_NOW`/`E:STOP`, botón E-STOP de la web, `SYS_DISARM`, watchdog, E-STOP físico o ultrasonido. Arranca el bloqueo de 5 s |
| `EVENT:CMD_EXECUTED:BRAKE_NOW` | Parada pedida por la web (`/driveStop`); también arranca el bloqueo de 5 s |
| `EVENT:CMD_EXECUTED:<SYS_ARM\|SYS_DISARM>` | Comando de armado aplicado (`SYS_ARM` se confirma siempre) |
| `EVENT:CMD_EXECUTED:SYS_MODE:<AUTO\|MANUAL>` | El modo cambió (no se emite si ya estaba en ese modo) |
| `EVENT:CMD_EXECUTED:<LIGHTS_ON\|LIGHTS_OFF\|LIGHTS_AUTO>` | El modo de luces cambió |
| `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE` | ARM con E-STOP accionado |
| `EVENT:CMD_REJECTED:SYS_MODE:BAD_VALUE` | Valor de modo no reconocido |
| `EVENT:ESTOP_TRIGGERED:GPIO` | E-STOP físico accionado |
| `EVENT:ESTOP_RELEASED` | E-STOP físico liberado |
| `EVENT:WATCHDOG_TIMEOUT` | Pérdida de enlace |
| `EVENT:FAULT_TRIGGERED:<motivo>` | Otra tarea forzó `FAULT`. Hoy el único motivo es `ULTRASONIC_OBSTACLE` (obstáculo a < 30 cm en 3 lecturas seguidas, ~150 ms) |
| `EVENT:FATAL:TASK_CREATE_FAILED:<tarea>` | Fallo de arranque; el equipo reinicia |
| `EVENT:FATAL:MAILBOX_INIT_FAILED` | Fallo de arranque; el equipo reinicia |
| `EVENT:FATAL:TX_QUEUE_CREATE_FAILED` | Fallo de arranque; el equipo reinicia |

`M:PING:0` no produce ningún evento, y los comandos ignorados por el estado (`DISARMED`/`FAULT`,
bloqueo de 5 s) tampoco: solo dejan una línea de depuración.

**Por dónde sale cada cosa.** Todos los eventos salen por el **USB** (115200). Por el **UART
externo** (GPIO 17/16, 921600) solo salen `EVENT:STATE_CHANGED` y `EVENT:MODE_CHANGED`: un cliente
conectado por el UART externo no ve los `CMD_RECEIVED`/`CMD_EXECUTED` ni los avisos de seguridad.

**Cada línea llega entera.** Toda la salida por USB pasa por `serial_line()`/`serial_linef()`
(`src/serial_out.h`), que arman la línea completa y la escriben de una sola vez bajo un mutex. Antes
de b4acb21 varias tareas imprimían a la vez y podían salir dos eventos pegados en una línea
(`EVENT:CMD_EXECUTED:EMERGENCY_BRAKEEVENT:STATE_CHANGED:FAULT`); eso ya no pasa, así que el cliente
puede parsear línea por línea sin heurísticas. Las líneas se truncan a ~157 caracteres, nunca se
parten.

> **Para quien toque el firmware:** cualquier línea nueva, de protocolo o de depuración, se imprime
> con `serial_line()`/`serial_linef()`. **Nunca** con `Serial.print`/`Serial.println` directo:
> `println` son dos escrituras y otra tarea puede meterse en el medio.

### Recuperación de FAULT

Se entra en `STATE_FAULT` por **watchdog** (pérdida de enlace, `EVENT:WATCHDOG_TIMEOUT`), por
**E-STOP físico** (`EVENT:ESTOP_TRIGGERED:GPIO`) o por el **ultrasonido** (obstáculo a menos de
30 cm durante 3 lecturas seguidas, `EVENT:FAULT_TRIGGERED:ULTRASONIC_OBSTACLE`). En los tres casos
se frena y se borra la velocidad latcheada; watchdog y E-STOP además centran la dirección.
FAULT bloquea todos los comandos de control y **no se borra solo**. Para recuperarse:

1. **Eliminar la causa.** Si fue el E-STOP, liberarlo (llega `EVENT:ESTOP_RELEASED`).
   Si fue el watchdog, restablecer el enlace. Si fue el ultrasonido, sacar el obstáculo: el
   firmware no lo comprueba al rearmar, así que si sigue ahí vuelve a `FAULT` a los ~150 ms.
2. Enviar `M:SYS_ARM:0`.
   - Si el E-STOP sigue accionado se responde `EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE`
     y el sistema **permanece** en FAULT.
   - Si la causa cesó, el sistema pasa a `ARMED` y responde `EVENT:CMD_EXECUTED:SYS_ARM`.
3. Mandar un `C:SET_SPEED` **nuevo** (no queda nada latcheado de antes del FAULT) y seguir
   alimentando el watchdog.

Rearmar es una decisión del cliente, no del firmware. Los clientes conocidos **no** rearman solos
tras un FAULT (ver §Clientes conocidos).

`M:SYS_DISARM:0` seguido de `M:SYS_ARM:0` también funciona y es la secuencia más
conservadora. Tras un frenado de emergencia hay un **bloqueo de 5 s** durante el cual
el motor no responde aunque el sistema esté armado.

El ultrasonido **funciona** y está siempre activo, en cualquier estado. (Durante un tiempo se creyó
que estaba muerto; en realidad había un obstáculo real a 22 cm delante del sensor.) Tenelo en
cuenta en el banco: una pared, una caja o una mano a menos de 30 cm lleva el auto a `FAULT`.

### Conexión y puerto serie
- **USB**: 115200 baud. Es por donde salen todos los eventos.
- **En la Jetson**, con el driver vendor de WCH, la placa aparece como **`/dev/ttyCH341USB0`**, no
  como `/dev/ttyUSB0`. Los ejemplos de este documento y de `test/` que dicen `/dev/ttyUSB0` hay que
  adaptarlos.
- **Abrir el puerto reinicia la placa** (llega de nuevo `EVENT:SYSTEM_READY`), aunque DTR/RTS
  queden en reposo. Hay que esperar ese evento antes de mandar comandos, y asumir que cada
  reconexión parte de cero: estado inicial, modo MANUAL y nada latcheado.

### Respuestas del ESP32
El ESP32 puede enviar mensajes de debug por serial. Puedes leerlos para debugging:

```python
if ser.in_waiting > 0:
    response = ser.readline().decode('utf-8', errors='ignore')
    print(f"ESP32: {response}")
```

### Manejo de Errores
- Si el comando no se parsea correctamente, el ESP32 imprime: `[LinkRxTask] Failed to parse message: ...`
- Si el canal no es `E`, `C` ni `M`: `[LinkRxTask] Unknown channel: <c>`. Un comando desconocido dentro de un canal válido se descarta **sin ningún mensaje** (y no alimenta el watchdog).
- Verifica que el formato sea exacto: `CHANNEL:COMMAND:VALUE\n`
- Asegúrate de que el baud rate coincida (115200 para USB, 921600 para UART externo)
- **UART externo: el enlace usa ahora GPIO 17 (TX) y GPIO 16 (RX).** Antes estaba asignado a
  GPIO 9/10, que en el ESP32-WROOM-32 pertenecen al flash SPI interno y ni siquiera están
  expuestos en el conector del DevKit v1. Si tenías cableado a 9/10, hay que recablear.

## Checklist de Integración

- [ ] Configurar conexión serial (USB o UART externo) y esperar `EVENT:SYSTEM_READY` al abrir el puerto
- [ ] Mandar `M:SYS_MODE` y `M:SYS_ARM:0` al conectar, esperando la confirmación de cada uno
- [ ] Implementar función de conversión grados → servo
- [ ] Alimentar el watchdog: algún comando válido a 10-20 Hz (`M:PING:0` o los de control)
- [ ] Manejar freno de emergencia en caso de detección de obstáculos
- [ ] Detectar `FAULT` (`EVENT:STATE_CHANGED:FAULT`) y decidir explícitamente si rearmar
- [ ] Probar con el simulador UART antes de integrar con lane detector

## Simulador de Pruebas

Puedes probar tus comandos con el simulador incluido:

```bash
cd embedded
pip install -r test/python/requirements.txt
python3 test/python/test_uart_simulator.py /dev/ttyCH341USB0 --baud 115200   # /dev/ttyUSB0 en una PC
```

Luego prueba comandos como:
- `C:SET_SPEED:120`
- `C:SET_STEER:105`
- `E:BRAKE_NOW:0`

Ojo: el simulador **no manda `M:PING:0`** por su cuenta. Escribiendo a mano es difícil mandar algo
cada menos de 1 s, así que después del primer comando el watchdog de MANUAL (1000 ms) suele llevar
el sistema a `FAULT` enseguida. Para probar con el auto andando, usá un script que mande PING a 10 Hz.

## Clientes conocidos

- **Brain-Aura** (`src/movement/esp32.py`): se apoya en el *latching* y mantiene el enlace con `M:PING:0` a 10 Hz.
- **`bfmc_hardware_bridge`** (nodo ROS2 del repo `Bosch-Aura/autonomous-vehicle-BFMC-ros2`): al abrir
  el puerto configura modo AUTO y arma; después manda `SET_STEER` + `SET_SPEED` a 20 Hz. Nunca rearma
  solo tras un `FAULT`.

## Soporte

Si tienes dudas sobre el protocolo o encuentras problemas, consulta:
- `embedded/src/link_rx_task.cpp` - Implementación del parser
- `embedded/include/messages.h` - Definiciones de canales y comandos
- `embedded/include/hardware.h` - Valores de servo (SERVO_CENTER, etc.)

## Configuracion compartida con Brain-Aura

Los valores que los **dos** lados tienen que ver igual viven en un solo archivo,
`Brain-Aura/hardware.env`. Cada clave declara ahi mismo a que `#define` del
firmware corresponde:

```
# firmware: SERVO_LEFT
BRAIN_SERVO_IZQUIERDA=50
```

`scripts/hardware_env.py` lo lee antes de compilar y emite `-DSERVO_LEFT=50`. Los
`#define` de `hardware.h` estan envueltos en `#ifndef`, asi que lo del archivo gana
y `hardware.h` queda solo como respaldo para compilar sin Brain-Aura al lado.

**El script no tiene ninguna lista de variables.** Para compartir una nueva alcanza
con marcarla en `hardware.env`; no hay que tocar el script. Todos los `#define` de
`include/hardware.h` estan envueltos en `#ifndef`, asi que cualquiera de ellos se
puede sobrescribir, incluidos los numeros de pin.

Si marcas una clave cuyo `#define` **no** este envuelto, el `-D` no le gana: el
codigo redefine el macro y `hardware.env` quedaria ignorado en silencio. El script
detecta ese caso y **corta la compilacion**, diciendo en que archivo esta el
`#define` y como envolverlo. Es el mismo criterio que con la jerarquia del
watchdog: mejor no compilar que flashear un auto cuyos numeros no coinciden con
los del que lo maneja.

Cambiar cualquiera de esos valores **exige recompilar y reflashear**. La jerarquia
`periodo < TTL < watchdog` y el orden `SERVO_LEFT < SERVO_CENTER < SERVO_RIGHT` se
comprueban al compilar: si el archivo dice algo incoherente, el firmware no compila.

