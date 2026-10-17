# Plan de Mejoras y Correcciones — Firmware VCU (ESP32 / FreeRTOS)

**Repositorio:** `embedded/` — PlatformIO / Arduino / ESP32 DevKit v1
**Rama analizada:** `main` (`3ad40f8`)
**Fecha:** 2026-08-04
**Alcance:** `src/*.cpp`, `include/*.h`, `platformio.ini`, `sdkconfig.defaults`, `docs/`

> **Este documento es un plan.** No modifica código. Cada fragmento de código es la corrección *propuesta*.

---

## 0. Estado de implementación (2026-08-04)

Rama `fix/plan-mejoras-fase0-fase2`. **Compila; NO probado en hardware.**

| Fase | Hallazgos | Estado |
|---|---|---|
| **Fase 0** | C-2, C-1, C-3, C-5, A-8, M-1, C-4 | ✅ **Implementada completa** |
| **Fase 1** | A-11, A-6, A-3, A-9, A-12, A-5, A-10 | ✅ **Implementada completa** |
| **Fase 2** | ítems 15-18 (A-13, M-10, A-1 doc, M-4/M-5/B-5) | ✅ Implementada (documentación) |
| **Fase 3** | A-2, A-1 (código), A-4, A-7, M-2, M-3, M-5…M-14, B-1…B-7 | ❌ No implementada |

**Adelantados desde Fase 3** (por ser dependencias directas de Fase 0/1):
- **M-12** — `*expired` siempre inicializado en `mailbox_read` (`src/mailbox.cpp`). Con C-1, un
  timeout de mutex ahora significa "frenar", que es el comportamiento correcto.
- **M-10** (parcial) — eliminado `DEFAULT_FORWARD_SPEED`, código muerto que sostenía C-1.

**Añadidos no previstos por el plan, requeridos por los cambios de Fase 0:**
- `src/webpage.cpp`: botones ARM / DISARM / E-STOP y visor de estado. Sin esto la UI web
  quedaba inutilizable tras C-3 (arranque `DISARMED`) porque **no existía ningún botón de armado**.
- `src/webpage.cpp`: keep-alive a 10 Hz del comando de velocidad. Tras C-1 los comandos
  expiran a los 200 ms y el auto frena; la UI enviaba un único AJAX por movimiento de slider.
- `src/web_task.cpp`: `supervisor_update_heartbeat()` en los handlers de control, exigido
  explícitamente por la nota de C-4.

**Pendiente de acción humana:**
- **A-10** — `include/wifi_data.h` sigue en el historial de git y en los clones existentes.
  **Hay que rotar la clave de la red `UA-Alumnos` a mano.** Borrar el archivo no basta.
- **A-11** — el cambio de UART1 a GPIO 16/17 **requiere recablear** el enlace con la Jetson.
- **A-13** — al corregir el rango del servo, el auto girará más a la derecha que antes con
  los mismos grados: **recalibrar el lazo de seguimiento de carril** y avisar al equipo Brain.

---

## 1. Resumen ejecutivo

Se revisó la totalidad del código fuente (2052 líneas, 12 `.cpp` + 5 `.h`). Se identificaron **39 hallazgos**: 5 críticos, 13 altos, 14 medios y 7 bajos.

El hallazgo dominante no es ninguno de los tres reportados inicialmente, sino uno derivado de ellos: **el vehículo no se detiene de forma fiable**. La lógica de *fallback* de `MotorTask` (`src/motor_task.cpp:190-198`) reaplica la última velocidad válida en cada ciclo de 10 ms sin consultar el estado del sistema. Como consecuencia, `M:SYS_DISARM:0` **no detiene el auto** (el `motor_stop()` del supervisor es sobreescrito ~10 ms después), y un frenado de emergencia solo lo detiene durante los 5 s de *cooldown*, tras los cuales **arranca solo a la última velocidad**, incluso estando en `STATE_FAULT`.

Combinado con que el sistema **arranca en `ARMED`→`RUNNING`** (contradiciendo el documento de protocolo) y con que **no existe watchdog en modo MANUAL** (el modo por defecto), el escenario del primer flasheo es: bootea listo para moverse, y ante una pérdida de enlace sigue acelerando indefinidamente.

Recomendación: **no flashear ni alimentar la etapa de potencia** hasta aplicar los 5 hallazgos críticos (§2). Se sugiere además hacer la primera prueba con el auto **sobre un soporte, con las ruedas al aire**.

### Dos correcciones al diagnóstico de partida

Durante la verificación, dos de los tres bugs reportados resultaron ser distintos de lo enunciado:

1. **El rango del servo NO es asimétrico.** `include/hardware.h:27-29` define `SERVO_LEFT 50`, `SERVO_CENTER 105`, `SERVO_RIGHT 160`. Los semi-rangos son `105-50 = 55` y `160-105 = 55`: **perfectamente simétrico**. El rango asimétrico es el del *documento* (50/105/135 → 55 y 30). Esto simplifica mucho la corrección (A-13).
2. **La función `degrees_to_servo()` del documento es incorrecta incluso respecto de sus propias constantes**, no solo respecto del código. Con `SERVO_RANGE = 85` y `SERVO_CENTER + n*(85/2)`, `degrees_to_servo(-45)` devuelve **63** (no 50, como afirma su propio comentario) y `degrees_to_servo(45)` devuelve **148** (no 135, y excede su propio máximo declarado). El documento está mal en dos niveles.

### Notación

- **[CONFIRMADO]** — verificado leyendo el código; se cita archivo:línea.
- **[SOSPECHA]** — razonamiento sólido pero requiere prueba en hardware o consulta de la versión de librería.

---

## 2. CRÍTICO — corregir ANTES del primer flasheo

### C-1. `DISARM` no detiene el vehículo; tras una emergencia el motor rearranca solo

**Archivos:** `src/motor_task.cpp:190-198`, `src/supervisor_task.cpp:75-85`
**Estado:** [CONFIRMADO]

El bloque de *fallback* de `MotorTask`:

```cpp
// src/motor_task.cpp:190-198
else if (has_received_speed_command)
{
    // Command expired, but maintain last valid speed (don't revert to default)
    current_speed = last_valid_speed;
    motor_set_direction(true);
    motor_set_speed(current_speed);
    ...
}
```

se ejecuta **sin consultar `supervisor_get_state()`**. La comprobación de estado (`can_control`, líneas 94-114) solo existe dentro de `case CMD_SET_SPEED`, es decir, únicamente cuando hay un comando *fresco* en el mailbox. En cuanto el comando expira (TTL 200 ms), el control pasa a este `else if`, que reaplica `last_valid_speed` en cada ciclo de 10 ms sin importar el estado.

Tres consecuencias concretas, todas verificadas por trazado:

**(a) `M:SYS_DISARM:0` no frena el auto.** El supervisor llama `motor_stop()` (`supervisor_task.cpp:78`) pero no notifica a `MotorTask` ni activa el *cooldown*. En el siguiente ciclo de `MotorTask` (≤10 ms) `in_cooldown` es `false`, el mailbox está expirado, `has_received_speed_command` sigue `true` → `motor_set_speed(last_valid_speed)`. **El auto sigue andando después de desarmarlo.**

**(b) Tras un frenado de emergencia, el auto rearranca a los 5 segundos.** La emergencia (E-STOP, `E:BRAKE_NOW`, watchdog, ultrasonido) preserva deliberadamente `last_valid_speed` y `has_received_speed_command` (comentario en `motor_task.cpp:49` y `:167`). Al expirar `STOP_COOLDOWN_MS`, `in_cooldown` pasa a `false` y el mismo `else if` reaplica la velocidad. **El vehículo se pone en marcha solo, sin ninguna orden, 5 s después de un E-STOP.**

**(c) `STATE_FAULT` no protege.** Tanto el watchdog (`supervisor_task.cpp:141`) como el E-STOP (`:121`) llevan a `STATE_FAULT`, pero el *fallback* ignora el estado. FAULT bloquea comandos *nuevos*, no el movimiento en curso.

Esto invalida la afirmación del `README.md` §1.1 de que el TTL "lleva el vehículo a un estado seguro ante la pérdida de enlace".

**Corrección propuesta** — el fallback debe ser *fail-safe* (frenar), y todo el lazo debe estar guardado por el estado:

```cpp
// src/motor_task.cpp — reemplazar el bloque 179-206
// Helper único de autorización, usado tanto para comandos nuevos como para el fallback
static bool motor_control_allowed(void)
{
    system_state_t state = supervisor_get_state();
    system_mode_t  mode  = supervisor_get_mode();
    if (mode == MODE_AUTO) {
        return (state == STATE_RUNNING);
    }
    return (state == STATE_ARMED || state == STATE_RUNNING);
}

// ... dentro del while(1), al aplicar el control:
if (in_cooldown || !motor_control_allowed())
{
    motor_stop();
    current_speed = 0;
    lights_set_reverse(false);
}
else if (has_valid_command)
{
    // ya aplicado arriba
}
else
{
    // FAIL-SAFE: sin comando fresco -> frenar. NO mantener la última velocidad.
    motor_stop();
    current_speed = 0;
    motor_direction = true;
    lights_set_reverse(false);
}
```

Y eliminar por completo las variables `last_valid_speed` y `has_received_speed_command` (`motor_task.cpp:24-25`), que solo existen para sostener el comportamiento peligroso.

Adicionalmente, `CMD_SYS_DISARM` debe notificar a `MotorTask` en lugar de manipular el hardware desde otra tarea:

```cpp
// src/supervisor_task.cpp:75-85
case CMD_SYS_DISARM:
    if (current_state != STATE_DISARMED) {
        current_state = STATE_DISARMED;
        motor_task_trigger_emergency();   // dueño del motor = MotorTask
        mailbox_write(steer_mb, TOPIC_STEER, CMD_STOP, 0, 100);
        ...
    }
    break;
```

> **Nota de diseño:** el comentario "don't revert to default" y el texto del protocolo ("el vehículo avanza automáticamente... para que siga moviéndose sin GPS cuando solo se controla la dirección") sugieren que este comportamiento fue **intencional**. Si el equipo lo quiere conservar para demos de *lane following*, debe quedar detrás de un flag de compilación explícito (`-DALLOW_SPEED_HOLD_ON_EXPIRY`), **desactivado por defecto**, y nunca activo en `STATE_FAULT` ni `STATE_DISARMED`.

---

### C-2. Punteros colgantes al stack de `setup()`

**Archivos:** `src/main.cpp:52-56`, `:105-108`, `:133-137`
**Estado:** [CONFIRMADO]

`link_rx_params`, `supervisor_params` y `web_params` son variables **locales** de `setup()`. Se pasa su dirección a `xTaskCreatePinnedToCore`:

```cpp
// src/main.cpp:52
link_rx_params_t link_rx_params = { ... };   // <-- local, vive en el stack de setup()
xTaskCreatePinnedToCore(link_rx_task, "LinkRxTask", STACK_SIZE_8K,
                        &link_rx_params, 4, NULL, 1);
```

Cuando `setup()` retorna, ese *stack frame* se libera y es reutilizado por `loopTask` (en Arduino-ESP32, `setup()` y `loop()` corren en la misma tarea, así que ese mismo stack se sobreescribe de inmediato). Las tres tareas siguen dereferenciando esos punteros — `LinkRxTask` en `link_rx_task.cpp:81-85`, `SupervisorTask` en `supervisor_task.cpp:25-28`, `WebTask` en `web_task.cpp:37-41`.

**Por qué importa:** comportamiento indefinido. Las tres tareas copian los punteros a estáticos *al inicio* de su cuerpo, así que si alcanzan a ejecutarse antes de que `setup()` retorne, "funciona". Es una carrera decidida por el planificador: `LinkRxTask` (prio 4) y `SupervisorTask`/`WebTask` (prio 2) se crean desde `loopTask` (prio 1) en el core 1 — las de prioridad mayor apropian de inmediato, las de prioridad 2 también. Es decir, **hoy probablemente funciona por casualidad**, y fallará ante cualquier cambio de prioridades, de orden de creación o de timing. El síntoma sería un `LoadProhibited` / `Guru Meditation` con direcciones basura, o peor, mailboxes apuntando a memoria arbitraria (escrituras silenciosas a RAM ajena).

Los mailboxes en sí (`main.cpp:17-20`) **sí** son correctos: son `static` a nivel de archivo.

**Corrección propuesta** — declararlas `static`:

```cpp
// src/main.cpp:52
static link_rx_params_t link_rx_params = {
    .motor_mailbox = &motor_mailbox,
    .steer_mailbox = &steer_mailbox,
    .lights_mailbox = &lights_mailbox,
    .supervisor_mailbox = &supervisor_mailbox};

// src/main.cpp:105
static supervisor_params_t supervisor_params = { ... };

// src/main.cpp:133
static web_task_params_t web_params = { ... };
```

Es una corrección de una palabra por sitio, sin riesgo. Alternativa más robusta (y preferible a mediano plazo): mover las estructuras a ámbito de archivo, junto a los mailboxes, para que la intención quede explícita.

---

### C-3. El sistema arranca ARMADO — contradice el documento de protocolo

**Archivos:** `src/supervisor_task.cpp:19-20`, `:64`, `:159-166`
**Estado:** [CONFIRMADO]

`docs/BRAIN_TEAM_PROTOCOL.md` afirma en negrita: *"**El sistema inicia DESARMADO por defecto.** Debes armar el sistema antes de que los comandos de control funcionen."* El código hace lo contrario:

```cpp
// src/supervisor_task.cpp:18-20
static system_mode_t  current_mode     = MODE_MANUAL;
static system_state_t current_state    = STATE_ARMED;   // <-- arranca ARMADO
static system_state_t previous_state   = STATE_ARMED;
```

Y como el modo por defecto es `MODE_MANUAL`, la primera iteración del lazo lo promueve a `RUNNING` sin ninguna intervención:

```cpp
// src/supervisor_task.cpp:159-166
} else {
    // In MANUAL mode, ARMED automatically transitions to RUNNING
    if (current_state != STATE_RUNNING) {
        current_state = STATE_RUNNING;
        ...
    }
}
```

**Consecuencia:** ~50 ms después del arranque el sistema está en `RUNNING`/`MANUAL`, es decir, aceptando comandos de tracción. Basta un `C:SET_SPEED:200` (o abrir la página web y mover el slider) para que el auto se mueva, sin haber armado nada. Con el auto sobre la mesa durante el primer flasheo, esto es un riesgo físico real.

**Bug asociado:** `CMD_SYS_ARM` solo actúa si el estado es `STATE_DISARMED`:

```cpp
// src/supervisor_task.cpp:63-73
case CMD_SYS_ARM:
    if (current_state == STATE_DISARMED) { ... }
    // If already armed, don't print again
    break;
```

Como nunca se arranca en `DISARMED`, **`M:SYS_ARM:0` es un no-op en el arranque** — no imprime `EVENT:CMD_EXECUTED:SYS_ARM`, con lo que un cliente que espere esa confirmación se cuelga. Peor: tras un `STATE_FAULT` (watchdog o E-STOP), `SYS_ARM` tampoco funciona; hay que hacer `SYS_DISARM` y después `SYS_ARM`. **Esta secuencia de recuperación no está documentada en ninguna parte.**

**Corrección propuesta** — alinear el código al documento (el documento es la interfaz publicada al equipo Brain; el código es lo que debe ceder):

```cpp
// src/supervisor_task.cpp:19-20
static system_state_t current_state  = STATE_DISARMED;
static system_state_t previous_state = STATE_DISARMED;
```

Y permitir el armado también desde `STATE_FAULT`, que es la transición de recuperación natural:

```cpp
// src/supervisor_task.cpp:63
case CMD_SYS_ARM:
    if (current_state == STATE_DISARMED || current_state == STATE_FAULT) {
        // Rearmar solo si la causa del fault ya no está presente
        if (estop_is_triggered()) {
            Serial.println("EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE");
            break;
        }
        current_state = STATE_ARMED;
        last_heartbeat_ms = current_ms;
        Serial.println("EVENT:CMD_EXECUTED:SYS_ARM");
        link_tx_send_state_event(current_state);
    } else {
        // Confirmar igual, para que el cliente no se quede esperando
        Serial.println("EVENT:CMD_EXECUTED:SYS_ARM");
    }
    break;
```

**Implicancias del cambio (importantes, evaluarlas con el equipo):**

- La página web (`src/web_task.cpp`) queda inutilizable hasta pulsar *arm*; hay que verificar que la UI (`src/webpage.cpp`) exponga ese botón de forma visible. El endpoint `/arm` ya existe (`web_task.cpp:171`).
- Todos los scripts de prueba existentes (`test/python/*.py`) deben enviar `M:SYS_ARM:0` antes de cualquier comando de control. El documento de protocolo ya lo indica, así que en teoría el equipo Brain ya lo hace.
- Este cambio **no sustituye** a C-1: aun arrancando `DISARMED`, el *fallback* de C-1 hace que un `DISARM` posterior no frene. Ambos son necesarios.

---

### C-4. Sin watchdog en modo MANUAL (el modo por defecto) — fuga ante pérdida de enlace

**Archivo:** `src/supervisor_task.cpp:132`
**Estado:** [CONFIRMADO]

```cpp
// src/supervisor_task.cpp:132
if (current_mode == MODE_AUTO && current_state != STATE_DISARMED) {
    // ... watchdog de heartbeat
}
```

El watchdog de enlace **solo existe en modo AUTO**. El modo por defecto es `MODE_MANUAL` (`supervisor_task.cpp:18`). Por lo tanto, en la configuración de arranque el sistema no tiene ninguna protección ante pérdida de enlace.

Combinado con C-1 (el motor mantiene la última velocidad al expirar el comando), la secuencia completa de fallo es:

1. Flashear → boot → `RUNNING` / `MANUAL` (C-3).
2. Enviar `C:SET_SPEED:200`.
3. Desconectar el cable USB / caerse el proceso de la Jetson.
4. El TTL de 200 ms expira → el *fallback* reaplica 200 → **el auto acelera indefinidamente sin nadie escuchando.**

Solo lo detiene el E-STOP físico (GPIO 4) o el ultrasonido — y ambos solo por 5 s (C-1b).

**Corrección propuesta** — el watchdog debe cubrir ambos modos, con timeouts diferenciados si se quiere ser permisivo con MANUAL (donde el operador humano es el lazo de control):

```cpp
// src/hardware.h — reemplazar el define único
#define WATCHDOG_TIMEOUT_AUTO_MS   300   // ver C-5 / A-3: > TTL de control
#define WATCHDOG_TIMEOUT_MANUAL_MS 1000  // más laxo: hay operador humano

// src/supervisor_task.cpp:131-146
if (current_state != STATE_DISARMED && current_state != STATE_FAULT) {
    uint32_t timeout = (current_mode == MODE_AUTO)
                     ? WATCHDOG_TIMEOUT_AUTO_MS
                     : WATCHDOG_TIMEOUT_MANUAL_MS;
    if (last_heartbeat_ms > 0 && (current_ms - last_heartbeat_ms) > timeout) {
        Serial.println("EVENT:WATCHDOG_TIMEOUT");
        current_state = STATE_FAULT;
        motor_task_trigger_emergency();
        mailbox_write(steer_mb, TOPIC_STEER, CMD_STOP, 0, 100);
    }
}
```

**Cuidado:** el control desde la página web **no actualiza el heartbeat** (`supervisor_update_heartbeat()` solo se llama desde `link_rx_task.cpp:107`). Si se activa el watchdog en MANUAL, hay que añadir la llamada en los *handlers* de `web_task.cpp`, o el auto entrará en FAULT mientras se lo maneja desde el navegador.

---

### C-5. Velocidad negativa o fuera de rango se convierte en velocidad alta

**Archivo:** `src/motor_task.cpp:127-131`
**Estado:** [CONFIRMADO]

```cpp
// src/motor_task.cpp:127-131
uint8_t new_speed = (uint8_t)value;      // value es int32_t
if (new_speed > MOTOR_SPEED_MAX)         // MOTOR_SPEED_MAX == 255
{
    new_speed = MOTOR_SPEED_MAX;
}
```

Dos defectos encadenados:

1. El *cast* a `uint8_t` ocurre **antes** de validar, truncando a los 8 bits bajos. `C:SET_SPEED:-50` → `(uint8_t)(-50)` = **206**. `C:SET_SPEED:300` → **44**. `C:SET_SPEED:256` → **0**.
2. El *clamp* es **código muerto**: un `uint8_t` nunca puede ser `> 255`. El compilador lo elimina (y con `-Wall` emitiría `comparison is always false`).

**Por qué importa:** un valor negativo por un bug en el lane detector aguas arriba (un `int` sin signo mal manejado, un ángulo/velocidad calculado como negativo) se traduce en el auto acelerando. `-1` → 255, velocidad máxima. Es exactamente el tipo de error que un rango mal validado convierte en accidente.

**Corrección propuesta** — validar en el tipo con signo, antes de convertir:

```cpp
// src/motor_task.cpp:127
if (value < 0) {
    value = 0;
} else if (value > MOTOR_SPEED_MAX) {
    value = MOTOR_SPEED_MAX;
}
uint8_t new_speed = (uint8_t)value;
```

Mejor aún: rechazar el comando en el borde del sistema, en `link_rx_task.cpp`, y no escribirlo nunca al mailbox (ver A-8), de modo que el valor inválido quede registrado como error de protocolo en lugar de silenciosamente saturado.

---

## 3. ALTO

### A-1. La respuesta de emergencia no es `<1 ms` — es de hasta ~40 ms

**Archivos:** `src/motor_task.cpp:39`, `:208`; `src/hardware.cpp:99`
**Estado:** [CONFIRMADO]

El `README.md` (§2.2) y `docs/BRAIN_TEAM_PROTOCOL.md` (canal `E`) afirman una respuesta de emergencia `<1 ms` mediante "despertar inmediato" de la tarea. El código no implementa eso:

```cpp
// src/motor_task.cpp:39  -- poll no bloqueante, timeout 0
uint32_t notification_value = ulTaskNotifyTake(pdTRUE, 0);
...
// src/motor_task.cpp:208  -- el bloqueo real
vTaskDelay(pdMS_TO_TICKS(MOTOR_TASK_PERIOD_MS));
```

`vTaskDelay()` **no** se interrumpe por `xTaskNotify()`. La notificación solo se ve en el siguiente despertar periódico. La latencia real de `E:BRAKE_NOW:0` es:

| Componente | Latencia |
|---|---|
| Polling de `LinkRxTask` (`link_rx_task.cpp:207`) | hasta 10 ms |
| Ciclo de `MotorTask` (`motor_task.cpp:208`) | hasta 10 ms |
| Bloqueo por `pulseIn()` del ultrasonido en el mismo core (A-2) | hasta 30 ms |
| **Total peor caso** | **~50 ms** |

A la velocidad máxima documentada, son decenas de centímetros de recorrido extra. La documentación entregada al equipo Brain declara una garantía que el firmware no cumple — es peligroso que integren asumiéndola.

**Corrección propuesta** — usar la notificación *como* mecanismo de espera del lazo, que es el patrón idiomático y elimina el `vTaskDelay`:

```cpp
// src/motor_task.cpp — reemplazar la línea 39 y la 208
while (1)
{
    // Bloquea hasta MOTOR_TASK_PERIOD_MS, PERO despierta al instante ante xTaskNotify().
    uint32_t notification_value =
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MOTOR_TASK_PERIOD_MS));

    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (notification_value > 0) {
        // ... frenado de emergencia (sin cambios)
    }

    // ... resto del cuerpo, sin vTaskDelay al final
}
```

Esto reduce la latencia de MotorTask a `<1 ms` real. Aplicar el mismo patrón, tal como propone `docs/v3.0.0/TODO.md`, al resto de los mailboxes elimina también los 10 ms de `LinkRxTask`.

---

### A-2. `pulseIn()` bloquea el core 0 hasta 30 ms desde la tarea de mayor prioridad

**Archivos:** `src/hardware.cpp:99`, `src/ultrasonic_task.cpp:17`, `:36`
**Estado:** [CONFIRMADO]

```cpp
// src/hardware.cpp:99
uint32_t duration = pulseIn(GPIO_ULTRASONIC_ECHO, HIGH, 30000); // 30ms timeout
```

`pulseIn()` es un *busy-wait* que no cede la CPU. Se ejecuta en `UltrasonicTask`, **prioridad 5, core 0** — la prioridad más alta del sistema. En el core 0 también viven `MotorTask` (prio 4) y `SteerTask` (prio 3).

**Consecuencia:** cada vez que el HC-SR04 no recibe eco (sin obstáculo dentro de ~5 m — es decir, **el caso normal**, no el excepcional), `pulseIn` consume los 30 ms completos de timeout con la CPU tomada. Durante esos 30 ms `MotorTask` y `SteerTask` **no se ejecutan**. Con un período de 50 ms en `UltrasonicTask`, el core 0 está bloqueado el **60 % del tiempo** en el escenario nominal.

Efectos: el lazo de control de 100 Hz de `MotorTask` (que el `README.md` §1.1 vende como "cadencia fija, determinista") en realidad pierde 3 de cada 5 ciclos; y la latencia del frenado de emergencia se degrada como se detalla en A-1.

Es una inversión de prioridades por diseño: la tarea de mayor prioridad es la que más bloquea, y bloquea a las de actuación.

**Corrección propuesta (por orden de preferencia):**

1. **Reemplazar `pulseIn` por interrupciones en el pin de eco.** El disparo y la medición se desacoplan; la tarea solo consume el resultado. Es la solución correcta.

```cpp
// src/hardware.cpp — esquema
static volatile uint32_t echo_start_us = 0;
static volatile uint32_t echo_width_us = 0;

static void IRAM_ATTR echo_isr(void) {
    if (digitalRead(GPIO_ULTRASONIC_ECHO)) {
        echo_start_us = micros();
    } else if (echo_start_us != 0) {
        echo_width_us = micros() - echo_start_us;
        echo_start_us = 0;
    }
}
// en hardware_init():
attachInterrupt(digitalPinToInterrupt(GPIO_ULTRASONIC_ECHO), echo_isr, CHANGE);
```

2. **Mitigación mínima si no se quiere refactorizar antes del flasheo:** bajar el timeout a lo que corresponde al umbral real de detección. El umbral es `ULTRASONIC_OBSTACLE_THRESHOLD_CM 30` (`hardware.h:47`); 30 cm ↔ ~1740 µs. Un timeout de 25 000 µs para un umbral de 30 cm es 14× lo necesario:

```cpp
// src/hardware.cpp:99 — mitigación
// 400 cm * 58 us/cm ~= 23200 us. Si solo importan 30 cm, alcanza con mucho menos.
uint32_t duration = pulseIn(GPIO_ULTRASONIC_ECHO, HIGH, 5000); // ~85 cm
```

3. **Mover `UltrasonicTask` al core 1** y bajarle la prioridad, para que su bloqueo no afecte al lazo de actuación (`main.cpp:150-158`). Es la mitigación de una línea, pero deja el *busy-wait* consumiendo CPU.

---

### A-3. Watchdog de 120 ms vs. TTL de control de 200 ms: incoherentes

**Archivos:** `include/hardware.h:50`, `src/supervisor_task.cpp:12`, `src/link_rx_task.cpp:128`, `:135`
**Estado:** [CONFIRMADO]

- Watchdog: `WATCHDOG_TIMEOUT_MS 120` — definido **dos veces**, en `hardware.h:50` y en `supervisor_task.cpp:12` (redefinición duplicada; hoy coinciden, pero es una bomba de tiempo para el mantenimiento).
- TTL de los comandos de control: `200` ms, *hardcodeado* en `link_rx_task.cpp:128` y `:135`.
- El documento recomienda enviar a 10–20 Hz, pero declara el mínimo como 5 Hz ("mínimo 5 Hz", `docs/BRAIN_TEAM_PROTOCOL.md` §TTL).

Los tres números son mutuamente inconsistentes:

1. **5 Hz (200 ms) rompe el watchdog.** El propio documento autoriza un período de 200 ms, que supera el timeout de 120 ms. Un cliente que siga el mínimo documentado dispara `EVENT:WATCHDOG_TIMEOUT` y va a `STATE_FAULT` en cada ciclo.
2. **El TTL de 200 ms es inalcanzable en modo AUTO.** Como el watchdog (120 ms) vence siempre primero, la expiración del TTL nunca llega a ocurrir sin que antes el sistema esté en FAULT. La lógica de expiración del mailbox es, en la práctica, **código muerto en AUTO**.
3. **Sin margen para el jitter.** Con `SUPERVISOR_TASK_PERIOD_MS 50` (`supervisor_task.cpp:11`), el watchdog se evalúa cada 50 ms: la detección real ocurre entre 120 y 170 ms. Un emisor a 10 Hz (100 ms, lo recomendado) queda a solo 20 ms de margen — un hipo de la Jetson, una retransmisión USB o un `Serial.flush()` largo (M-3) lo hacen saltar.

**Corrección propuesta** — hacer que las constantes se deriven unas de otras y respeten la jerarquía `período_emisión < TTL < watchdog`:

```cpp
// include/hardware.h — única fuente de verdad para el timing del enlace
#define LINK_EXPECTED_PERIOD_MS  100                            // 10 Hz, lo recomendado al equipo Brain
#define CONTROL_CMD_TTL_MS       (LINK_EXPECTED_PERIOD_MS * 2)  // 200 ms
#define WATCHDOG_TIMEOUT_AUTO_MS (LINK_EXPECTED_PERIOD_MS * 3)  // 300 ms > TTL
```

Y:
- Eliminar la redefinición de `supervisor_task.cpp:12`; incluir `hardware.h` (que ya se incluye, línea 2).
- Sustituir los `200` literales de `link_rx_task.cpp:128,135` por `CONTROL_CMD_TTL_MS`.
- Bajar `SUPERVISOR_TASK_PERIOD_MS` a 20 ms para reducir el jitter de detección.
- Actualizar `docs/BRAIN_TEAM_PROTOCOL.md`: el mínimo pasa a ser 10 Hz, no 5 Hz.

---

### A-4. Estado compartido entre núcleos sin `volatile` ni sección crítica

**Archivos:** `src/supervisor_task.cpp:18-22`, `:186-192`; consumidores en `src/motor_task.cpp:94-95` y `src/steer_task.cpp:35-36`
**Estado:** [CONFIRMADO] (la ausencia de sincronización). [SOSPECHA] (que llegue a manifestarse un fallo observable)

```cpp
// src/supervisor_task.cpp:18-22 — sin volatile, sin mutex
static system_mode_t  current_mode      = MODE_MANUAL;
static system_state_t current_state     = STATE_ARMED;
static system_state_t previous_state    = STATE_ARMED;
static uint32_t       last_heartbeat_ms = 0;
static bool           estop_triggered   = false;
```

No hay una sola aparición de `volatile` en todo el proyecto (verificado por `grep`). Escenario de acceso cruzado:

- `current_state` / `current_mode`: **escritas** por `SupervisorTask` (**core 1**), **leídas** por `MotorTask` y `SteerTask` (**core 0**) vía `supervisor_get_state()` / `supervisor_get_mode()`.
- `last_heartbeat_ms`: **escrita** por `LinkRxTask` (core 1) vía `supervisor_update_heartbeat()` (`:182-184`), leída y escrita por `SupervisorTask` (core 1).
- `tx_queue` (`link_tx_task.cpp:26`): escrita por `LinkTxTask`, leída por `SupervisorTask` (ver A-5).

**Por qué importa:** sin `volatile`, el compilador puede mantener el valor en un registro dentro del lazo `while(1)` de `MotorTask` y **no releerlo nunca** de memoria. Un `DISARM` emitido desde el core 1 podría no ser visto jamás por el core 0. Que hoy funcione depende de que las llamadas a `Serial.print` y `vTaskDelay` actúen como barreras de optimización accidentales — no es una garantía, y desaparece si se sube el nivel de optimización o se refactoriza.

En ESP32 (Xtensa LX6, dual-core con caches coherentes por hardware para DRAM interna) la coherencia de caché no es el problema; el problema es el **compilador** y el **reordenamiento**.

Nota: las variables son `int`/`enum`/`bool` de 32 bits y naturalmente alineadas, así que las lecturas/escrituras individuales son atómicas en la práctica. No hay riesgo de *tearing*; el riesgo es de visibilidad y de **inconsistencia entre variables** (leer `current_state` y `current_mode` en dos instrucciones separadas puede tomar un par no coherente en `motor_task.cpp:94-95`, justo durante una transición).

**Corrección propuesta:**

```cpp
// src/supervisor_task.cpp:18-22
static volatile system_mode_t  current_mode      = MODE_MANUAL;
static volatile system_state_t current_state     = STATE_DISARMED;  // ver C-3
static volatile system_state_t previous_state    = STATE_DISARMED;
static volatile uint32_t       last_heartbeat_ms = 0;
static volatile bool           estop_triggered   = false;
```

Para la lectura consistente del par `(estado, modo)`, exponer un único getter atómico en lugar de dos:

```cpp
// src/supervisor_task.h
typedef struct { system_state_t state; system_mode_t mode; } supervisor_snapshot_t;
supervisor_snapshot_t supervisor_get_snapshot(void);

// src/supervisor_task.cpp
supervisor_snapshot_t supervisor_get_snapshot(void) {
    supervisor_snapshot_t s;
    portENTER_CRITICAL(&supervisor_spinlock);   // static portMUX_TYPE, spinlock inter-core
    s.state = current_state;
    s.mode  = current_mode;
    portEXIT_CRITICAL(&supervisor_spinlock);
    return s;
}
```

`portENTER_CRITICAL`/`portEXIT_CRITICAL` con un `portMUX_TYPE` es el mecanismo correcto para exclusión mutua **entre núcleos** en ESP32 (a diferencia de `taskENTER_CRITICAL`, que en un puerto de un solo core solo deshabilita interrupciones).

> **Sobre los mailboxes:** `src/mailbox.cpp` **sí** tiene protección real. Usa un `SemaphoreHandle_t` mutex (`mailbox.cpp:13`) tomado en `mailbox_write` (`:24`) y `mailbox_read` (`:44`) con timeout de 10 ms. Los mutex de FreeRTOS en el puerto ESP32 son seguros entre núcleos. La ruta de datos de comandos está bien; el problema es el estado del supervisor, que **no** pasa por mailbox.

---

### A-5. `tx_queue` se crea dentro de `LinkTxTask` — carrera de inicialización, eventos perdidos

**Archivos:** `src/link_tx_task.cpp:26`, `:29`; `src/supervisor_task.cpp:33`, `:47-48`
**Estado:** [CONFIRMADO]

```cpp
// src/link_tx_task.cpp:28-29
void link_tx_task(void *pvParameters) {
    tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(telemetry_msg_t));
```

La cola se crea **dentro del cuerpo de la tarea**, no en `setup()`. Los productores comprueban `if (tx_queue != NULL)` (`:74`, `:86`, `:98`) y, si es `NULL`, **descartan el mensaje en silencio**.

`SupervisorTask` intenta compensarlo con un `vTaskDelay(pdMS_TO_TICKS(100))` (`supervisor_task.cpp:33`) y el comentario *"Give other tasks time to initialize (especially link_tx_task)"* — es decir, el propio autor detectó la carrera y la parcheó con un *sleep*. Un delay fijo no es una sincronización: es una apuesta.

Además, `LinkTxTask` (prio 2, core 1) se crea **después** de `SupervisorTask` (prio 2, core 1) en `main.cpp` (líneas 109 y 121). Con igual prioridad y *round-robin*, el orden no está garantizado. Y `tx_queue` no es `volatile` (A-4).

**Consecuencia:** los `EVENT:STATE_CHANGED` / `EVENT:MODE_CHANGED` iniciales pueden no salir nunca por `Serial1`. El equipo Brain no recibiría el estado inicial y quedaría sin saber si el auto está armado.

**Corrección propuesta** — crear la cola en `setup()`, antes de crear ninguna tarea:

```cpp
// src/link_tx_task.h
void link_tx_init(void);   // crear la cola antes de arrancar las tareas

// src/link_tx_task.cpp
void link_tx_init(void) {
    tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(telemetry_msg_t));
    configASSERT(tx_queue != NULL);
}
void link_tx_task(void *pvParameters) {
    // ya no crea la cola
    Serial.println("[LinkTxTask] LinkTx task started");
    while (1) { ... }
}

// src/main.cpp, junto a mailbox_init() (línea ~45)
link_tx_init();
```

Con eso, el `vTaskDelay(100)` de `supervisor_task.cpp:33` deja de ser necesario y puede eliminarse (ver M-1: recorta 100 ms del arranque).

---

### A-6. El parser no acumula líneas: mensajes fragmentados se pierden o se malinterpretan

**Archivo:** `src/link_rx_task.cpp:66-78`, `:91-96`
**Estado:** [CONFIRMADO]

```cpp
// src/link_rx_task.cpp:66-78
static int read_line_from(Stream &stream, char *buffer, int max_len) {
    int len = 0;
    while (stream.available() > 0 && len < max_len - 1) {
        buffer[len++] = stream.read();
        if (buffer[len - 1] == '\n' || buffer[len - 1] == '\r') break;
    }
    ...
}
```

El lazo termina cuando `available()` llega a 0, **no cuando aparece el terminador**. No hay estado persistente entre invocaciones: cada llamada empieza con `len = 0`.

**Consecuencia:** si en el instante del *poll* solo han llegado `C:SET_S` de `C:SET_SPEED:120\n`, la función devuelve ese fragmento; `parse_uart_message` lo procesa (tiene ≥3 caracteres y contiene `:`), extrae `cmd = "SET_S"`, no coincide con nada, y **el comando se pierde silenciosamente**. El resto (`PEED:120\n`) se lee en la siguiente vuelta y produce `[LinkRxTask] Failed to parse message` con canal `P` desconocido.

Peor: **el heartbeat se actualiza igual** (`link_rx_task.cpp:107`), porque `parse_uart_message` devolvió `true`. Un flujo de basura mantiene vivo el watchdog.

Con `vTaskDelay(10 ms)` (`:207`) y 921 600 baudios en `Serial1`, en 10 ms caben ~1150 bytes: la probabilidad de cortar un mensaje por la mitad es alta y crece con la tasa de envío.

**Corrección propuesta** — acumulador persistente por stream, con emisión solo al ver el terminador:

```cpp
// src/link_rx_task.cpp — sustituye read_line_from
typedef struct {
    char buf[UART_BUF_SIZE];
    int  len;
} line_accum_t;

// Devuelve true y deja la línea completa (sin terminador) en acc->buf
static bool read_line_accum(Stream &stream, line_accum_t *acc) {
    while (stream.available() > 0) {
        char c = (char)stream.read();
        if (c == '\n' || c == '\r') {
            if (acc->len == 0) continue;      // ignorar terminadores sueltos
            acc->buf[acc->len] = '\0';
            return true;                      // línea completa
        }
        if (acc->len < (int)sizeof(acc->buf) - 1) {
            acc->buf[acc->len++] = c;
        } else {
            acc->len = 0;                     // línea demasiado larga: descartar
            Serial.println("[LinkRxTask] Line overflow, discarded");
        }
    }
    return false;                             // aún incompleta
}
// Uso: static line_accum_t acc_usb, acc_uart1;  (uno por stream, no compartido)
```

Y mover `supervisor_update_heartbeat()` (`:107`) a **después** de haber despachado con éxito un comando de un canal conocido, para que la basura no mantenga vivo el watchdog.

**Relacionado:** `LinkRxTask` lee primero de `Serial` (USB) y solo si no hay nada de `Serial1` (`:93-96`). Como `Serial` es también el canal por el que las 8 tareas escupen depuración, el USB nunca está "vacío" en el sentido de haber cesado — pero eso es TX, no RX, así que no interfiere. Sí conviene documentar que ambos enlaces están activos simultáneamente y que un comando puede entrar por cualquiera: no hay arbitraje ni prioridad declarada entre los dos, y un `E:BRAKE_NOW` por `Serial1` puede quedar detrás de un comando de USB.

---

### A-7. La marcha atrás no funciona: `MotorTask` fuerza "adelante" en cada ciclo

**Archivos:** `src/web_task.cpp:60-66`, `:147-151`; `src/motor_task.cpp:139`, `:150`, `:194`
**Estado:** [CONFIRMADO]

Los *handlers* web fijan el sentido de giro llamando directamente al hardware:

```cpp
// src/web_task.cpp:60-66
server.on("/back", []() {
    if (motor_mb != NULL) {
        mailbox_write(motor_mb, TOPIC_MOTOR, CMD_SET_SPEED, MOTOR_SPEED_MAX, 100);
        motor_set_direction(false);          // <-- marcha atrás
    }
    ...
});
```

Pero `MotorTask` reafirma "adelante" **en todas** sus rutas de aplicación de velocidad:

```cpp
// src/motor_task.cpp:139, :150, :194
motor_set_direction(true);
```

**Consecuencia:** el `motor_set_direction(false)` del *handler* web (que corre en `WebTask`, core 1) es revertido por `MotorTask` (core 0) dentro de los 10 ms siguientes. **La marcha atrás es imposible** desde la web, tanto por `/back` como por `/changeSpeed?direction=backward` (`web_task.cpp:147-148`). El slider de la UI (`webpage.cpp:252-263`) ofrece un rango de −255 a +255 con etiqueta "Atrás", que no hace lo que dice. Por la misma razón, las luces de reversa nunca se encienden: `lights_set_reverse(false)` aparece en todas las rutas y `lights_set_reverse(true)` **no aparece nunca** en el código.

Además es una violación de propiedad: dos tareas en dos núcleos distintos escriben los mismos GPIO (`GPIO_MOTOR_IN3`/`IN4`) sin sincronización.

**Corrección propuesta** — el sentido debe viajar *dentro* del comando, y solo `MotorTask` debe tocar el motor. La forma menos invasiva es codificar el sentido en el signo del valor del mailbox:

```cpp
// src/web_task.cpp — el handler solo escribe al mailbox, nunca al hardware
server.on("/back", []() {
    if (motor_mb != NULL) {
        mailbox_write(motor_mb, TOPIC_MOTOR, CMD_SET_SPEED, -MOTOR_SPEED_MAX, 100);
    }
    server.send(200, "text/plain", "back");
});

// src/motor_task.cpp — interpretar el signo (reemplaza el clamp de C-5)
bool want_forward = (value >= 0);
int32_t magnitude = want_forward ? value : -value;
if (magnitude > MOTOR_SPEED_MAX) magnitude = MOTOR_SPEED_MAX;
uint8_t new_speed = (uint8_t)magnitude;

motor_set_direction(want_forward);
motor_set_speed(new_speed);
lights_set_reverse(!want_forward);
motor_direction = want_forward;
```

**Ojo:** esto entra en conflicto con la corrección de C-5 tal como está escrita (que satura los negativos a 0). Hay que **decidir la semántica del signo antes de implementar ambas**:
- Si `C:SET_SPEED` es **magnitud sin signo** (lo que dice hoy el protocolo: "0-255"), aplicar C-5 tal cual y añadir un comando separado `C:SET_DIR:<0|1>`.
- Si `C:SET_SPEED` pasa a ser **con signo** (−255..255), aplicar esta variante y **actualizar `docs/BRAIN_TEAM_PROTOCOL.md`**, que hoy documenta 0-255.

Recomendación: signo, es más simple y elimina un comando. Pero es un cambio de protocolo y hay que avisarle al equipo Brain.

---

### A-8. Ángulo de dirección negativo se convierte en giro máximo a la derecha

**Archivo:** `src/steer_task.cpp:60-66`
**Estado:** [CONFIRMADO]

```cpp
// src/steer_task.cpp:60-66
uint16_t new_angle = (uint16_t)value;      // value es int32_t
if (new_angle < SERVO_LEFT)  new_angle = SERVO_LEFT;
else if (new_angle > SERVO_RIGHT) new_angle = SERVO_RIGHT;
```

Mismo defecto de C-5 aplicado a la dirección. `C:SET_STEER:-30` → `(uint16_t)(-30)` = **65506** → mayor que `SERVO_RIGHT` → *clampeado* a **160 = giro máximo a la DERECHA**.

**Por qué importa:** es el caso realista si el equipo Brain envía grados en vez del valor de servo — que es exactamente lo que el documento advierte que no hay que hacer, y precisamente por eso incluye `degrees_to_servo()`. Un `-30` (30° a la izquierda) produce **giro completo al lado contrario**. En un auto siguiendo carril, invertir la dirección es la peor falla posible.

**Corrección propuesta:**

```cpp
// src/steer_task.cpp:60
if (value < SERVO_LEFT) {
    value = SERVO_LEFT;
} else if (value > SERVO_RIGHT) {
    value = SERVO_RIGHT;
}
uint16_t new_angle = (uint16_t)value;
```

Y, mejor aún, rechazar en el borde (`link_rx_task.cpp`) los valores fuera de `[SERVO_LEFT, SERVO_RIGHT]` con un `EVENT:CMD_REJECTED:SET_STEER:OUT_OF_RANGE`, para que el error se detecte aguas arriba en lugar de saturarse en silencio.

---

### A-9. `M:SYS_MODE:AUTO` selecciona MANUAL en silencio

**Archivo:** `src/link_rx_task.cpp:156-171`
**Estado:** [CONFIRMADO]

```cpp
// src/link_rx_task.cpp:158-164
// Value can be "AUTO"/"MANUAL" as string or 0/1 as integer
int32_t mode;
if (value == 1 || value == 0) {
    mode = (value == 1) ? MODE_AUTO : MODE_MANUAL;
} else {
    mode = MODE_AUTO; // Default to AUTO if not clear
}
```

El comentario promete aceptar cadenas, pero `value` viene de `atoi()` (`:53`), que devuelve **0** para cualquier texto no numérico. Por lo tanto:

- `M:SYS_MODE:AUTO` → `atoi("AUTO")` = 0 → `value == 0` → **`MODE_MANUAL`**. Exactamente lo contrario de lo pedido, sin ningún error reportado.
- `M:SYS_MODE:MANUAL` → 0 → MANUAL (correcto por casualidad).
- La rama `else` (línea 162-164) es **inalcanzable** para cadenas; solo se activa con enteros distintos de 0 y 1 (`2`, `-1`, `7`), donde asume AUTO — un valor inválido no debería asumir el modo *menos* supervisado.

**Consecuencia:** el equipo Brain cree estar en AUTO (con watchdog activo, ver C-4) y en realidad está en MANUAL (sin watchdog). El síntoma sería exactamente el escenario de fuga de C-4, y sería muy difícil de diagnosticar porque el emisor cree haber configurado lo correcto.

Nota: `web_task.cpp:165` sí compara la cadena correctamente (`value_str == "AUTO"`), así que la web y la UART se comportan distinto ante la misma entrada.

**Corrección propuesta** — comparar la cadena de verdad y rechazar lo desconocido:

```cpp
// src/link_rx_task.cpp — requiere que parse_uart_message devuelva también el texto crudo del valor
} else if (strcmp(cmd, "SYS_MODE") == 0 && supervisor_mb != NULL) {
    int32_t mode;
    if (strcmp(value_str, "AUTO") == 0 || strcmp(value_str, "1") == 0) {
        mode = MODE_AUTO;
    } else if (strcmp(value_str, "MANUAL") == 0 || strcmp(value_str, "0") == 0) {
        mode = MODE_MANUAL;
    } else {
        Serial.println("EVENT:CMD_REJECTED:SYS_MODE:BAD_VALUE");
        break;   // no asumir nada
    }
    mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_MODE, mode, 5000);
}
```

Esto requiere modificar `parse_uart_message` (`:21`) para devolver también el valor como cadena, además de como entero.

---

### A-10. Credenciales Wi-Fi reales versionadas en el repositorio

**Archivo:** `include/wifi_data.h:1-3`
**Estado:** [CONFIRMADO]

```c
// Replace the values below with your network credentials.
#define SSID "UA-Alumnos"
#define PASSWORD "41umn05WLC"
```

El archivo **está en el índice de git** (`git ls-files` lo confirma) y presente en el historial desde al menos `6487b17`. El remoto es `git@github.com:Robot-Autonomo-de-Laboratorio-BFMC/embedded.git`. Es la contraseña de la red Wi-Fi institucional, no un placeholder.

Agravante: `grep` confirma que **`wifi_data.h` no se incluye desde ningún archivo**. `web_task.cpp` levanta un AP propio con sus propias constantes (`web_task.cpp:13-14`). Es decir, **el secreto está expuesto sin aportar ninguna función**.

**Corrección propuesta:**

1. Eliminar el archivo del árbol de trabajo y del índice, y añadirlo a `.gitignore`:

```bash
git rm --cached include/wifi_data.h
rm include/wifi_data.h
printf '\n# Credenciales locales\ninclude/wifi_data.h\n' >> .gitignore
```

2. Versionar en su lugar una plantilla `include/wifi_data.h.example` con valores ficticios.
3. **Rotar la contraseña de la red `UA-Alumnos`** o reportarlo a quien administre la red. Borrar el archivo no lo quita del historial de git ni de los clones ya hechos.
4. Si se decide purgar el historial (`git filter-repo`), coordinar con todo el equipo: reescribe los hashes y obliga a reclonar.

---

### A-11. UART1 asignado a GPIO 9 y 10 — pines del flash SPI interno

**Archivos:** `include/hardware.h:37-38`, `src/hardware.cpp:38`
**Estado:** [CONFIRMADO] (la asignación). [SOSPECHA] (el fallo concreto depende del modo de flash y del módulo)

```c
// include/hardware.h:37-38
#define UART_TX_PIN 9
#define UART_RX_PIN 10
```

```cpp
// src/hardware.cpp:38
Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
```

En el ESP32-WROOM-32 (el módulo del DevKit v1) los **GPIO 6–11 están conectados a la memoria flash SPI integrada**. GPIO 9 = `SD_DATA2`, GPIO 10 = `SD_DATA3`. En modo QIO/QOUT se usan para datos; en DIO/DOUT quedan nominalmente libres pero **siguen físicamente unidos a los pines `/WP` y `/HOLD` del chip de flash**.

**Consecuencias posibles:** desde que la comunicación no funcione (los pines ni siquiera están expuestos en el conector de 30 pines del DevKit v1 estándar), hasta cuelgues aleatorios, `Guru Meditation` por fallo de lectura de flash, o corrupción durante el flasheo.

Este es un candidato fuerte a explicar por qué el enlace con la Jetson no está funcionando, independientemente del firmware viejo que hay flasheado.

**Corrección propuesta** — reasignar a pines libres y expuestos. En el DevKit v1, GPIO 16 y 17 son la elección estándar para UART2/UART1:

```c
// include/hardware.h:37-38
#define UART_TX_PIN 17
#define UART_RX_PIN 16
```

**Verificar antes de aplicar:**
- Que 16/17 no colisionen con el cableado físico actual del auto.
- Que el módulo no sea una variante con PSRAM (en ESP32-WROVER, GPIO 16/17 están tomados por la PSRAM; en ese caso usar 18/19 o 32/33 — pero 32/33 ya están ocupados por las luces, `hardware.h:18-19`).
- Confirmar el `board_build.flash_mode` efectivo del proyecto.

---

### A-12. El valor de retorno de `xTaskCreatePinnedToCore` nunca se verifica

**Archivo:** `src/main.cpp:57`, `:69`, `:81`, `:93`, `:109`, `:121`, `:138`, `:150` (8 llamadas)
**Estado:** [CONFIRMADO]

Las ocho llamadas ignoran el retorno. `xTaskCreatePinnedToCore` devuelve `pdPASS` o `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`.

El presupuesto de stack es 2×8 K + 6×4 K = **40 KB**, más el heap de Wi-Fi (`WebTask` levanta un SoftAP: fácilmente 40–50 KB) y del servidor HTTP. En un ESP32 con ~290 KB de DRAM útil no debería fallar, pero si falla:

**Consecuencia:** `main.cpp` imprime igual `"[main] XxxTask created..."` y `"[main] System ready!"` (líneas 66, 78, …, 162), y `EVENT:SYSTEM_READY`. El sistema **anuncia estar listo mientras una tarea no existe**. Si la que falta es `MotorTask`, nadie lee el mailbox de motor y el auto simplemente no responde; si es `SupervisorTask`, no hay watchdog ni E-STOP y **nada lo detiene**. Un fallo silencioso en el arranque de una tarea de seguridad es inaceptable.

**Corrección propuesta** — verificar y detener el arranque de forma ruidosa:

```cpp
// src/main.cpp — helper
static void create_task_or_die(TaskFunction_t fn, const char *name, uint32_t stack,
                               void *params, UBaseType_t prio, BaseType_t core)
{
    BaseType_t ok = xTaskCreatePinnedToCore(fn, name, stack, params, prio, NULL, core);
    if (ok != pdPASS) {
        Serial.printf("EVENT:FATAL:TASK_CREATE_FAILED:%s\n", name);
        Serial.flush();
        // Estado seguro antes de reiniciar
        motor_stop();
        delay(100);
        esp_restart();
    }
    Serial.printf("[main] %s created on Core %d, Priority %d\n", name, (int)core, (int)prio);
}

// Uso:
create_task_or_die(link_rx_task, "LinkRxTask", STACK_SIZE_8K, &link_rx_params, 4, 1);
create_task_or_die(motor_task,   "MotorTask",  STACK_SIZE_4K, &motor_mailbox,  4, 0);
// ... etc.
```

Aplicar lo mismo a `xSemaphoreCreateMutex()` en `mailbox.cpp:13`: hoy solo imprime un mensaje (`:15`) y continúa, dejando un mailbox donde **todo `mailbox_write` y `mailbox_read` retorna `false` en silencio** (`mailbox.cpp:20-22`, `:39-41`) — es decir, un canal de comandos permanentemente muerto que no se anuncia como tal.

---

### A-13. El documento de protocolo declara un rango de servo que no existe

**Archivos:** `docs/BRAIN_TEAM_PROTOCOL.md` (sección `C:SET_STEER`) vs. `include/hardware.h:27-29`
**Estado:** [CONFIRMADO]

El documento declara:

```python
SERVO_LEFT = 50
SERVO_CENTER = 105
SERVO_RIGHT = 135        # <-- el código dice 160
SERVO_RANGE = 85         # <-- 135 - 50
```

El código define `SERVO_RIGHT 160` (`include/hardware.h:29`). Dos defectos independientes:

**(a) El tope derecho está mal.** Un cliente que use el documento nunca supera 135, quedándose en el **55 % del recorrido disponible a la derecha** (`(135-105)/(160-105)`). El auto gira notablemente menos hacia la derecha que hacia la izquierda, y el equipo lo atribuiría a un problema mecánico del tren delantero en lugar de a un error de documentación.

**(b) La función del documento es incorrecta incluso respecto de sus propias constantes.** `degrees_to_servo()` calcula `SERVO_CENTER + normalized * (SERVO_RANGE / 2)` = `105 + n*42.5`:

| Entrada | Lo que el comentario del doc afirma | Lo que la función del doc realmente devuelve | Correcto según `hardware.h` |
|---|---|---|---|
| `-45°` | 50 | **63** | 50 |
| `+45°` | 135 | **148** (excede su propio máximo declarado) | 160 |
| `0°` | 105 | 105 | 105 |

Es decir, el documento está mal en dos niveles a la vez, y ninguna de sus dos versiones (constantes vs. función) coincide con la otra ni con el firmware.

**(c) Corolario importante:** el rango real **es simétrico** (`105 - 50 = 55` y `160 - 105 = 55`). El rango asimétrico es el inventado por el documento (55 a la izquierda, 30 a la derecha). Esto hace que la corrección sea trivial: basta un semi-rango único de 55.

**Fuente de verdad:** `include/hardware.h:27-29`. Es lo que el firmware ejecuta (`steer_task.cpp:62-66` *clampea* contra esas constantes) y lo que la interfaz web ya implementa correctamente (`src/webpage.cpp:153-168`). El documento es el único artefacto equivocado.

**Corrección propuesta:** ver el código completo y la advertencia de coordinación en §6, Fase 2, punto 15.

Además, para que esto no vuelva a divergir, `docs/BRAIN_TEAM_PROTOCOL.md` no debería repetir las constantes a mano. Exponerlas por telemetría al arrancar, de modo que el cliente las lea del propio firmware:

```cpp
// src/main.cpp, junto a EVENT:SYSTEM_READY
Serial.printf("EVENT:SERVO_RANGE:%d:%d:%d\n", SERVO_LEFT, SERVO_CENTER, SERVO_RIGHT);
```

---

## 4. MEDIO

### M-1. `delay(10000)` bloquea el arranque 10 segundos

**Archivo:** `src/main.cpp:32`
**Estado:** [CONFIRMADO]

```cpp
// src/main.cpp:30-32
// Wait for serial monitor to connect (ESP32 Serial is always available,
// but we need time for the monitor to open)
delay(10000);
```

Diez segundos de espera fija antes de inicializar el hardware. Problemas:

- Durante esos 10 s **el hardware no está inicializado**: `hardware_init()` (línea 39) no ha corrido, así que `GPIO_MOTOR_IN3/IN4/ENB` están en estado de reset (entradas flotantes). Según el puente H, un pin flotante puede leerse como alto y **hacer girar el motor al arrancar**. Es un peligro real, no teórico: la inicialización a estado seguro debe ser lo primero, no lo que sigue a una espera de 10 s.
- Es una espera fija para un problema (que el monitor serie alcance a conectarse) que solo se da en desarrollo. En operación con la Jetson, retrasa 10 s cada reinicio.
- Es el marcador de diagnóstico que se usó para confirmar que la ESP32 tiene firmware viejo (el servo se engancha a 45 ms en vez de ~10 045 ms). Al aplicar la corrección, esa señal desaparece: conviene dejar constancia.

**Corrección propuesta:**

```cpp
// src/main.cpp:26-39
void setup(void)
{
    // 1. PRIMERO: hardware a estado seguro. Antes que cualquier cosa.
    hardware_init();

    Serial.begin(115200);

#ifdef WAIT_FOR_SERIAL_MONITOR
    delay(3000);   // solo en builds de desarrollo, y con el hardware ya seguro
#endif

    Serial.println("========================================");
    ...
}
```

Y en `platformio.ini`, un entorno de desarrollo aparte:

```ini
[env:esp32doit-devkit-v1-debug]
extends = env:esp32doit-devkit-v1
build_flags = ${env:esp32doit-devkit-v1.build_flags} -DWAIT_FOR_SERIAL_MONITOR
```

Combinado con eliminar el `vTaskDelay(100)` de `supervisor_task.cpp:33` (ver A-5), el arranque baja de ~10,1 s a menos de 200 ms.

---

### M-2. Ocho tareas escriben a `Serial` directamente: la telemetría se entrelaza y se corrompe

**Archivos:** todos los `src/*.cpp` (`main.cpp` 15 prints, `link_rx_task.cpp` 25, `supervisor_task.cpp` 24, `motor_task.cpp` 11, `web_task.cpp` 6, `steer_task.cpp` 6, `lights_task.cpp` 4, `ultrasonic_task.cpp` 4, `hardware.cpp` 1, `mailbox.cpp` 1)
**Estado:** [CONFIRMADO] (el patrón). [SOSPECHA] (la frecuencia real de corrupción)

La arquitectura define `LinkTxTask` (`src/link_tx_task.cpp`) precisamente como el punto único de salida de telemetría, con una cola. Pero **todas las demás tareas lo puentean** y llaman `Serial.print`/`println` directamente, incluyendo los eventos del protocolo:

```cpp
// src/motor_task.cpp:143-145 — evento de protocolo emitido fuera de LinkTxTask
Serial.print("EVENT:CMD_EXECUTED:SET_SPEED:");
Serial.println(current_speed);
Serial.flush();
```

`Serial.println(x)` no es una operación atómica: son varias llamadas a `write()`. Con 8 tareas en 2 núcleos emitiendo concurrentemente, **las líneas se entrelazan a mitad de camino**. Ejemplo de salida posible:

```
EVENT:CMD_EXECUTED:SET_[SupervisorTask] System DISARMED
SPEED:120
```

**Consecuencia:** el consumidor (`test/python/telemetry_monitor.py`, el dashboard, el equipo Brain) parsea líneas rotas. Un `EVENT:` partido se pierde; y si lo que se parte es `EVENT:ESTOP_TRIGGERED` o `EVENT:WATCHDOG_TIMEOUT`, se pierde un aviso de seguridad. Además, `LinkRxTask` lee comandos del **mismo** `Serial`, así que el canal está saturado de depuración compitiendo con los comandos.

**Corrección propuesta:**

1. **Separar protocolo de depuración.** Los `EVENT:` van exclusivamente por `LinkTxTask`; la depuración humana va a un macro que se compila fuera en producción:

```cpp
// include/log.h (nuevo)
#ifdef ENABLE_DEBUG_LOG
  #define LOG(...)  do { Serial.printf(__VA_ARGS__); } while (0)
#else
  #define LOG(...)  do { } while (0)
#endif
```

2. **Ampliar `LinkTxTask`** con un tipo de mensaje genérico de evento, para que los `EVENT:` de cualquier tarea se serialicen por la cola:

```cpp
// src/link_tx_task.h
void link_tx_send_event(const char *fmt, ...);   // encola, no bloquea, no imprime
```

3. Si por ahora se quiere una solución mínima sin refactor, envolver cada emisión en un mutex global de `Serial` y construir la línea completa con `snprintf` antes de un único `Serial.write()`.

---

### M-3. `Serial.flush()` en el camino caliente

**Archivos:** `src/link_rx_task.cpp` (12 ocurrencias), `src/motor_task.cpp`, `src/supervisor_task.cpp`, `src/steer_task.cpp`, `src/link_tx_task.cpp:55`, `:57`, `:64`, `:66`
**Estado:** [CONFIRMADO]

`Serial.flush()` bloquea la tarea hasta que se vacía el FIFO de transmisión. A 115 200 baudios son ~87 µs por byte: una línea de 40 caracteres cuesta **~3,5 ms**.

`LinkRxTask` hace `flush()` después de **cada** comando recibido (`link_rx_task.cpp:115`, `:126`, `:133`, `:145`, …). A 20 Hz, con 2 prints por comando, son ~7 ms por comando gastados esperando al UART, dentro de la tarea de prioridad 4 responsable de recibir el freno de emergencia.

`LinkTxTask` hace `flush()` sobre `Serial` **y** sobre `Serial1` por cada evento (`:55-57`, `:64-66`), duplicando el costo.

**Consecuencia:** latencia añadida en la ruta de comandos, incluida la de emergencia; y contribuye al jitter que hace saltar el watchdog (A-3).

**Corrección propuesta:** eliminar todos los `flush()` salvo el último de `setup()` (`main.cpp:164`), donde sí interesa garantizar que el banner salga antes de que arranquen las tareas. El driver UART transmite igual de forma asíncrona; `flush()` no hace que salga *antes*, solo hace que el llamador *espere*.

---

### M-4. `STATE_FAULT` no tiene ruta de recuperación documentada

**Archivos:** `src/supervisor_task.cpp:64`, `:121`, `:141`, `:148-167`
**Estado:** [CONFIRMADO]

`STATE_FAULT` se alcanza por E-STOP (`:121`) o por watchdog (`:141`). La máquina de estados (`:149`) solo transiciona **desde `STATE_ARMED`**, así que FAULT es absorbente. Y `CMD_SYS_ARM` solo actúa desde `STATE_DISARMED` (`:64`).

La única salida es `SYS_DISARM` (que sí acepta cualquier estado ≠ DISARMED, `:76`) y luego `SYS_ARM`. Esta secuencia **no aparece en `docs/BRAIN_TEAM_PROTOCOL.md`**. Un operador que vea `EVENT:STATE_CHANGED:FAULT` no tiene forma de saber cómo recuperar.

Además, `E-STOP` limpia `estop_triggered` al soltarse (`:124-129`) pero **no restaura el estado**: queda en FAULT aunque la causa ya no exista.

**Corrección propuesta:** aplicar el cambio de `CMD_SYS_ARM` propuesto en C-3 (permitir armar desde FAULT si la causa cesó) y añadir al documento de protocolo una sección "Recuperación de FAULT" con la secuencia y las precondiciones.

---

### M-5. El ultrasonido dispara la emergencia sin considerar el estado del sistema

**Archivo:** `src/ultrasonic_task.cpp:19-30`
**Estado:** [CONFIRMADO]

`UltrasonicTask` llama `motor_task_trigger_emergency()` sin consultar `supervisor_get_state()`. Efectos:

- **Estando `DISARMED` o parado**, un obstáculo a <30 cm (una pared, la mano de alguien, el borde de la mesa) genera un frenado de emergencia y arranca un *cooldown* de 5 s, con su ruido en el log. No es peligroso, pero confunde el diagnóstico durante el desarrollo.
- **Bloqueo indefinido:** tras disparar, el contador se resetea a 0 (`:29`) y vuelve a acumular. Con el obstáculo presente, se redispara cada 150 ms (3 × 50 ms), reiniciando el *cooldown* de 5 s **perpetuamente**. El auto queda inmovilizado mientras haya algo enfrente y no hay ningún evento que lo explique — el `Serial.print` de `:25-27` no emite un `EVENT:` del protocolo, así que el equipo Brain no ve nada. (El comportamiento en sí es defendible; lo que falta es la telemetría.)
- Combinado con C-1b, el patrón real ante un obstáculo persistente es: frena 5 s → arranca solo contra el obstáculo → frena a los 150 ms → repite. **El auto embiste el obstáculo a intervalos.**

**Corrección propuesta:**

```cpp
// src/ultrasonic_task.cpp:19
if (distance_cm > 0 && distance_cm < ULTRASONIC_OBSTACLE_THRESHOLD_CM) {
    obstacle_detected_count++;
    if (obstacle_detected_count >= ULTRASONIC_DEBOUNCE_COUNT) {
        // Emitir evento de protocolo, no solo depuración
        Serial.print("EVENT:OBSTACLE_DETECTED:");
        Serial.println(distance_cm);
        // Solo actuar si el vehículo puede estar en movimiento
        system_state_t st = supervisor_get_state();
        if (st == STATE_ARMED || st == STATE_RUNNING) {
            motor_task_trigger_emergency();
        }
        obstacle_detected_count = 0;
    }
}
```

Y documentar `EVENT:OBSTACLE_DETECTED:<cm>` en `docs/BRAIN_TEAM_PROTOCOL.md`, junto con `EVENT:ESTOP_TRIGGERED`, `EVENT:ESTOP_RELEASED` y `EVENT:WATCHDOG_TIMEOUT`, ninguno de los cuales figura hoy.

---

### M-6. `sdkconfig.defaults` no tiene efecto con `framework = arduino`

**Archivos:** `sdkconfig.defaults`, `platformio.ini:12`
**Estado:** [SOSPECHA] (alta confianza; verificar con la versión concreta de la plataforma)

El archivo declara configuración de ESP-IDF:

```
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_TASK_WDT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
...
```

Con `framework = arduino` en PlatformIO, el core Arduino-ESP32 se consume **precompilado**: `sdkconfig.defaults` se ignora por completo (solo aplica a `framework = espidf` o a arduino-como-componente). Es decir, **ninguna de estas opciones está activa**.

Da una falsa sensación de configuración. En particular:

- `CONFIG_FREERTOS_HZ=1000` da la casualidad de coincidir con el valor por defecto de Arduino-ESP32, así que no hay divergencia práctica — pero es coincidencia, no configuración.
- `CONFIG_ESP_TASK_WDT=y` sugiere que hay un Task Watchdog de hardware vigilando. **No lo hay**, y aunque lo hubiera, ninguna tarea se suscribe con `esp_task_wdt_add()`. Confiar en un watchdog inexistente es peor que no tenerlo.
- `CONFIG_UART1_BAUD_RATE` no es una opción real de IDF que afecte a `Serial1.begin()`.

**Corrección propuesta:** o bien eliminar `sdkconfig.defaults` y trasladar lo que importe a `platformio.ini` (`board_build.f_cpu`, `build_flags`), o bien migrar a `framework = espidf` con Arduino como componente. Lo mínimo: añadir un comentario al principio del archivo advirtiendo que es inerte con la configuración actual, para que nadie lo lea como fuente de verdad.

Si se quiere un Task Watchdog real, con Arduino-ESP32 se hace en código:

```cpp
#include "esp_task_wdt.h"
// en setup(), tras crear las tareas:
esp_task_wdt_init(5, true);          // 5 s, reinicio ante panic
// y en el lazo de cada tarea crítica:
esp_task_wdt_add(NULL);              // una vez, al entrar
esp_task_wdt_reset();                // en cada iteración
```

---

### M-7. `platform = espressif32` sin versión fijada

**Archivo:** `platformio.ini:11`
**Estado:** [CONFIRMADO]

```ini
platform = espressif32
```

Sin `@version`, PlatformIO resuelve a la última disponible en cada máquina y en cada momento. La transición de Arduino-ESP32 2.x a 3.x cambió el core de forma incompatible (API de LEDC, `analogWrite`, gestión de timers del servo). Dos integrantes del equipo pueden obtener binarios distintos del mismo commit, y un `pio pkg update` puede romper la compilación sin que nada haya cambiado en el repo.

Esto es especialmente relevante porque `hardware.cpp:50` usa `analogWrite` y `ESP32Servo@^3.0.7` (también con `^`, `platformio.ini:22`) interactúa con el mismo periférico LEDC (ver M-8).

**Corrección propuesta:**

```ini
platform = espressif32@6.9.0     ; fijar tras verificar cuál se está usando hoy
lib_deps =
    madhephaestus/ESP32Servo@3.0.7   ; sin ^, versión exacta
```

Determinar la versión actualmente instalada con `pio pkg list` antes de fijarla, para no cambiar de comportamiento al mismo tiempo que se estabiliza.

---

### M-8. Posible conflicto de timers LEDC entre `ESP32Servo` y `analogWrite`

**Archivos:** `src/hardware.cpp:33-35` (servo), `:50` (`analogWrite`)
**Estado:** [SOSPECHA] — requiere verificación en hardware

```cpp
// src/hardware.cpp:33-35
steerServo.setPeriodHertz(SERVO_PWM_FREQ_HZ);   // 50 Hz
steerServo.attach(GPIO_SERVO, 500, 2500);
```

```cpp
// src/hardware.cpp:50
analogWrite(GPIO_MOTOR_ENB, speed);
```

Ambos usan el periférico LEDC del ESP32. `ESP32Servo` toma un timer LEDC a 50 Hz; `analogWrite` de Arduino-ESP32 asigna su propio canal/timer (1000 Hz por defecto). Si acaban compartiendo timer, **la frecuencia de uno pisa la del otro**: el servo a 1 kHz no funciona (vibra o se va a un extremo), o el motor a 50 Hz zumba audiblemente y pierde par.

Adicionalmente, `grep` confirma que **no se llama a `ESP32PWM::allocateTimer()`**, que los ejemplos de ESP32Servo 3.x recomiendan invocar antes de `attach()` precisamente para reservar timers de forma explícita y evitar esta colisión.

Este es un candidato razonable para explicar el comportamiento del servo observado en el firmware viejo, aunque no se puede afirmar sin probar.

**Corrección propuesta:**

```cpp
// src/hardware.cpp, en hardware_init(), ANTES de attach()
ESP32PWM::allocateTimer(0);   // reservar explícitamente para el servo
ESP32PWM::allocateTimer(1);
steerServo.setPeriodHertz(SERVO_PWM_FREQ_HZ);
steerServo.attach(GPIO_SERVO, 500, 2500);
```

Y, para el motor, sustituir `analogWrite` por LEDC explícito con canal y frecuencia elegidos a mano, sin depender del asignador automático:

```cpp
// src/hardware.cpp
#define MOTOR_PWM_CHANNEL 4      // fuera del rango que toma ESP32Servo
#define MOTOR_PWM_FREQ    1000
#define MOTOR_PWM_RES     8

// en hardware_init():
ledcSetup(MOTOR_PWM_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
ledcAttachPin(GPIO_MOTOR_ENB, MOTOR_PWM_CHANNEL);

void motor_set_speed(uint8_t speed) {
    ledcWrite(MOTOR_PWM_CHANNEL, speed);
}
```

(La API de `ledcSetup`/`ledcAttachPin` cambió en Arduino-ESP32 3.x a `ledcAttach(pin, freq, res)`. Ajustar según la versión que se fije en M-7.)

**Verificación sugerida:** medir con osciloscopio la frecuencia en `GPIO_SERVO` (debe ser 50 Hz) y en `GPIO_MOTOR_ENB` tras un `C:SET_SPEED:128`.

---

### M-9. `GPIO 12` (MOTOR_IN4) es un pin de *strapping*

**Archivo:** `include/hardware.h:15`
**Estado:** [CONFIRMADO] (que es strapping). [SOSPECHA] (que cause un fallo con este cableado)

```c
#define GPIO_MOTOR_IN4 12
```

GPIO 12 es `MTDI`, pin de *strapping* del ESP32: su nivel en el instante del reset selecciona la tensión del regulador del flash (bajo → 3,3 V; **alto → 1,8 V**). Si el driver del puente H tiene un *pull-up* en esa entrada, o si la etapa de potencia está alimentada y fuerza el pin alto durante el arranque, **el ESP32 configura el flash a 1,8 V y no arranca** (o arranca de forma inestable).

El código lo pone en `LOW` (`hardware.cpp:27`), pero eso ocurre *después* del boot: el *strapping* ya se leyó.

Agrava el problema M-1: durante los 10 s de `delay()` el pin está sin configurar.

**Corrección propuesta:** preferible reasignar `GPIO_MOTOR_IN4` a un pin sin función de strapping (por ejemplo GPIO 18, 19, 21, 22 o 23, todos libres en este diseño). Si no se puede recablear, verificar con un multímetro que GPIO 12 está por debajo de 0,8 V durante el reset con la etapa de potencia alimentada, y añadir un *pull-down* de 10 kΩ a masa.

Otros pines de strapping en uso o cercanos, a verificar: GPIO 2 (`GPIO_LED_BUILTIN`, `hardware.h:22`) también es strapping, pero debe estar bajo o flotante en el arranque y el LED integrado ya lo lleva a masa por el propio LED — riesgo bajo.

---

### M-10. `DEFAULT_FORWARD_SPEED` definido y nunca usado; el documento menciona un tercer valor

**Archivos:** `src/motor_task.cpp:12`, `docs/BRAIN_TEAM_PROTOCOL.md` (sección `C:SET_SPEED`)
**Estado:** [CONFIRMADO]

```cpp
// src/motor_task.cpp:12
#define DEFAULT_FORWARD_SPEED 220 // Default speed when no command received (0-255)
```

`grep` confirma que es su **única aparición en todo el repositorio**. Es código muerto.

Divergencia a tres bandas sobre el mismo comportamiento:

| Fuente | Qué dice |
|---|---|
| `docs/BRAIN_TEAM_PROTOCOL.md` | "el vehículo avanza automáticamente a velocidad **100**" |
| `src/motor_task.cpp:12` | `DEFAULT_FORWARD_SPEED` = **220** (sin usar) |
| `src/motor_task.cpp:190-198` | Mantiene la **última velocidad** recibida |

Ninguna de las tres coincide. El comportamiento real es el tercero, y es el que C-1 propone eliminar.

**Corrección propuesta:** al aplicar C-1, borrar `DEFAULT_FORWARD_SPEED` (`motor_task.cpp:12`) y corregir el párrafo del documento para que diga lo que va a ser cierto: *"Si el comando expira o se pierde el enlace, el vehículo frena. No hay velocidad por defecto."*

---

### M-11. AP Wi-Fi abierto con endpoints de control sin autenticación

**Archivo:** `src/web_task.cpp:13-14`, `:22-24`, `:171-188`
**Estado:** [CONFIRMADO]

```cpp
#define WIFI_AP_SSID "RC-Car-ESP32"
#define WIFI_AP_PASSWORD ""  // Open AP
```

Red abierta, sin contraseña. Cualquiera dentro del alcance puede conectarse y hacer `GET /arm`, `GET /changeSpeed?speed=255`, `GET /disarm`, `GET /brake` — control total del vehículo, incluida la tracción a fondo. No hay autenticación, ni token, ni límite de tasa, ni comprobación de origen.

En un laboratorio compartido o en una competencia, es un vector de interferencia trivial (accidental o no).

**Corrección propuesta (mínimo viable):**

```cpp
#define WIFI_AP_SSID     "RC-Car-ESP32"
#define WIFI_AP_PASSWORD "<clave WPA2 de al menos 8 caracteres>"   // no versionar
```

Y aplicar el mismo tratamiento que en A-10: la clave va en un `wifi_data.h` **ignorado por git**, con una plantilla `.example` versionada. Adicionalmente, considerar limitar el AP a 1 cliente (`WiFi.softAP(ssid, pass, channel, false, 1)`) y desactivar `WebTask` por completo en las compilaciones de competencia mediante un flag de build.

---

### M-12. `mailbox_read` no inicializa `*expired` cuando la entrada no es válida

**Archivo:** `src/mailbox.cpp:38-60`
**Estado:** [CONFIRMADO] (el defecto). Impacto bajo hoy, pero es una trampa.

```cpp
// src/mailbox.cpp:44-58
if (xSemaphoreTake(mb->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (mb->valid) {                       // <-- si es false, *expired no se escribe
        *expired = mailbox_is_expired(mb, current_ms);
        ...
    }
    xSemaphoreGive(mb->mutex);
}
return result;
```

Si `mb->valid` es `false` (mailbox nunca escrito) o si el mutex no se obtiene en 10 ms, `*expired` **queda sin inicializar**. Los llamadores declaran `bool expired;` sin inicializar (`motor_task.cpp:81`, `steer_task.cpp:28`, `lights_task.cpp:36`, `supervisor_task.cpp:58`) y leen un valor basura.

Hoy no se manifiesta porque todos los llamadores comprueban primero el retorno (`if (mailbox_read(...))`) y solo miran `expired` dentro. Pero es una API que promete escribir un parámetro de salida y no siempre lo hace: el próximo que la use de otra forma se lleva el bug.

**Corrección propuesta:**

```cpp
// src/mailbox.cpp:43
bool result = false;
if (expired != NULL) *expired = true;      // por defecto: pesimista
if (xSemaphoreTake(mb->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    ...
}
```

Nota relacionada: si el mutex no se obtiene en 10 ms, `mailbox_read` devuelve `false` y `MotorTask` lo interpreta como "no hay comando", cayendo al *fallback*. Con la corrección de C-1 eso significa **frenar**, que es el comportamiento correcto ante un fallo de sincronización. Vale la pena contar esos casos y emitir un evento si son frecuentes.

---

### M-13. Centinelas basados en "valor distinto de 0" y aritmética de tiempo

**Archivos:** `src/motor_task.cpp:57`, `src/supervisor_task.cpp:133`, `:152`
**Estado:** [CONFIRMADO] (el patrón). Probabilidad de manifestarse: muy baja.

**Buena noticia primero:** la aritmética de tiempo del proyecto es **correcta**. Todos los cálculos de edad usan resta sin signo sobre `uint32_t`:

```cpp
uint32_t age_ms = current_ms - mb->ts_ms;        // mailbox.cpp:71
uint32_t heartbeat_age = current_ms - last_heartbeat_ms;   // supervisor_task.cpp:134
uint32_t elapsed = current_time - last_stop_timestamp;     // motor_task.cpp:59
```

La resta sin signo en complemento a dos da el intervalo correcto **incluso a través del desbordamiento** de `xTaskGetTickCount() * portTICK_PERIOD_MS` (a `CONFIG_FREERTOS_HZ=1000` y `portTICK_PERIOD_MS=1`, ocurre cada ~49,7 días). No hay bug de overflow aquí. Conviene dejarlo escrito para que nadie lo "arregle" introduciendo comparaciones con signo.

**Lo que sí es frágil** son los centinelas que usan `0` como "sin valor":

```cpp
if (last_stop_timestamp > 0)     // motor_task.cpp:57
if (last_heartbeat_ms > 0)       // supervisor_task.cpp:133, :152
```

En el tick 0 (y una vez cada 49,7 días) un timestamp legítimo vale exactamente 0 y se confunde con "sin valor". Consecuencia en `motor_task.cpp:57`: un frenado que ocurriera justo en ese tick no activaría el *cooldown*. Es una ventana de 1 ms cada 49,7 días — irrelevante en la práctica, pero gratis de arreglar.

**Corrección propuesta:** un booleano explícito en lugar de un valor mágico.

```cpp
// src/motor_task.cpp
static bool     stop_pending = false;
static uint32_t last_stop_timestamp = 0;
// ...
if (stop_pending) {
    if ((current_time - last_stop_timestamp) >= STOP_COOLDOWN_MS) {
        stop_pending = false;
        in_cooldown  = false;
    } else {
        in_cooldown = true;
    }
}
```

Lo mismo con `last_heartbeat_ms` → `bool heartbeat_received`.

---

### M-14. `strdup`/`free` en el camino caliente del parser

**Archivo:** `src/link_rx_task.cpp:22`, `:35`, `:43`, `:61`
**Estado:** [CONFIRMADO]

```cpp
// src/link_rx_task.cpp:22
char *msg_copy = strdup(msg);
```

`strdup` hace `malloc` en cada mensaje recibido. A 20 Hz son 20 asignaciones/segundo en la tarea que atiende el freno de emergencia.

Problemas: (a) fragmentación del heap a largo plazo; (b) latencia no determinista de `malloc` (toma el mutex global del heap, compartido con Wi-Fi y el servidor HTTP de `WebTask`, que asigna intensamente); (c) si `malloc` falla, se devuelve `false` (`:23-25`) y el comando se pierde en silencio — incluido `E:BRAKE_NOW`.

La ruta de liberación **sí es correcta**: los tres `return` (`:24`, `:36`, `:44`) liberan antes de salir, y el camino feliz también (`:61`). No hay fuga. El problema es usar el heap, no cómo se usa.

**Corrección propuesta** — buffer en stack, sin asignación dinámica:

```cpp
// src/link_rx_task.cpp:21
static bool parse_uart_message(const char *msg, char *channel, char *cmd,
                               char *value_str, size_t value_str_len, int32_t *value)
{
    char buf[UART_BUF_SIZE];
    size_t n = strlen(msg);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, msg, n + 1);
    // ... resto idéntico, operando sobre buf, sin free()
}
```

`UART_BUF_SIZE` es 1024 (`hardware.h:39`) y el stack de `LinkRxTask` es 8 KB (`main.cpp:60`), así que entra con margen. Aun así, conviene reducir `UART_BUF_SIZE` a 128: ningún comando del protocolo pasa de 32 caracteres, y 1024 bytes por buffer (hay dos con la corrección de A-6) es desperdicio.

---

## 5. BAJO / ESTILO

### B-1. Comentario obsoleto sobre el rango del servo
`src/web_task.cpp:80` — `// Clamp angle to valid range (50-135)`, pero el código *clampea* a `SERVO_RIGHT` = 160 (`:82`). El comentario arrastra el valor viejo del documento. Corregir a `(50-160)`.

### B-2. Variable `TAG` sin usar
`src/hardware.cpp:5` — `static const char *TAG = "hardware";` no se referencia nunca (residuo de un port desde ESP-IDF, donde lo usaría `ESP_LOGI`). Eliminar, o adoptar el logging de IDF de forma consistente.

### B-3. `link_tx_send_status` y `MSG_TYPE_STATUS` son código muerto
`src/link_tx_task.cpp:73-83` — `grep` confirma que `link_tx_send_status` no se llama desde ningún sitio. El `case MSG_TYPE_STATUS` (`:44-47`) hace `continue` sin hacer nada. El campo `heartbeat_age_ms` de `telemetry_msg_t` (`:23`) es, por tanto, inútil. Eliminar los tres, o implementar `M:GET_STATUS:0` — que el propio comentario de `supervisor_task.cpp:175` menciona como la alternativa, pero **que no está implementado en `link_rx_task.cpp`** (no hay ninguna rama `GET_STATUS`). El comentario documenta una función inexistente.

### B-4. `data[len] = '\0'` redundante
`src/link_rx_task.cpp:99` — `read_line_from` ya termina el buffer en `:75`. Inofensivo (`len` está acotado), pero duplicado. Desaparece al aplicar A-6.

### B-5. `EVENT:SYSTEM_READY` no está en el protocolo
`src/main.cpp:163` emite `EVENT:SYSTEM_READY`, pero `docs/BRAIN_TEAM_PROTOCOL.md` no documenta ningún evento de salida. El equipo Brain no tiene forma de saber qué esperar. Documentar el catálogo completo de eventos: `SYSTEM_READY`, `STATE_CHANGED`, `MODE_CHANGED`, `CMD_RECEIVED:*`, `CMD_EXECUTED:*`, `ESTOP_TRIGGERED`, `ESTOP_RELEASED`, `WATCHDOG_TIMEOUT`.

### B-6. Flags de compilación sin usar
`platformio.ini:19-21` define `-DUART_BAUD=921600` y `-DSERIAL_BAUD=115200`. `grep` confirma que **ningún archivo los usa**: el código usa `UART_BAUD_RATE` de `hardware.h:36` y el literal `115200` en `main.cpp:28`. Tres fuentes de verdad para dos baudrates. Unificar: que `hardware.h` los tome de los flags, o eliminar los flags y dejar solo `hardware.h`.

### B-7. `lights_task`: `last_ldr_check` estático y el primer chequeo
`src/lights_task.cpp:21`, `:92` — `last_ldr_check` empieza en 0, así que la condición `current_ms - last_ldr_check >= LDR_CHECK_PERIOD_MS` es verdadera en la primera evaluación tras 1 s de uptime; funciona por casualidad. Además, con `LIGHTS_TASK_PERIOD_MS` = 1000 y `LDR_CHECK_PERIOD_MS` = 1000, la comprobación de período interna es redundante: el propio `vTaskDelay` ya impone la cadencia. Simplificar eliminando `last_ldr_check`.

---

## 6. Orden de ejecución sugerido

### Fase 0 — OBLIGATORIO ANTES DEL PRIMER FLASHEO

> **Seguridad física del vehículo.** No conectar la batería a la etapa de potencia hasta completar esta fase. Hacer la primera prueba con el auto sobre un soporte, ruedas al aire, y con el E-STOP (GPIO 4) verificado a mano *antes* de armar.

| # | Hallazgo | Archivo | Esfuerzo | Por qué antes de flashear |
|---|---|---|---|---|
| 1 | **C-2** Punteros colgantes | `main.cpp:52,105,133` | 3 líneas | Comportamiento indefinido en 3 tareas, incluida la de seguridad. Trivial de arreglar, sin excusa para postergarlo. |
| 2 | **C-1** El fallback ignora el estado | `motor_task.cpp:190-198` | ~30 líneas | `DISARM` no frena y el auto rearranca solo 5 s después de un E-STOP. Es *el* bug del que todo lo demás depende. |
| 3 | **C-3** Arranca ARMADO | `supervisor_task.cpp:19-20,64` | ~10 líneas | Bootea listo para moverse. Contradice el contrato publicado al equipo Brain. |
| 4 | **C-5** Velocidad negativa → 206 | `motor_task.cpp:127` | 5 líneas | Un `-1` aguas arriba se convierte en velocidad máxima. |
| 5 | **A-8** Ángulo negativo → derecha máxima | `steer_task.cpp:60` | 5 líneas | Dirección invertida: la peor falla en seguimiento de carril. |
| 6 | **M-1** `delay(10000)` antes de `hardware_init()` | `main.cpp:32,39` | 5 líneas | Los pines del motor quedan flotantes 10 s en el arranque. Invertir el orden. |
| 7 | **C-4** Sin watchdog en MANUAL | `supervisor_task.cpp:132` | ~15 líneas | Sin esto, la pérdida de enlace en el modo por defecto no tiene ninguna red de contención. |

**Verificación de la Fase 0 (banco, ruedas al aire), en este orden:**
1. Al arrancar: llega `EVENT:STATE_CHANGED:DISARMED`. Enviar `C:SET_SPEED:150` → **el motor no gira**.
2. `M:SYS_ARM:0` → llega `EVENT:CMD_EXECUTED:SYS_ARM`. `C:SET_SPEED:150` → gira.
3. `M:SYS_DISARM:0` → **el motor se detiene y sigue detenido** (esto es lo que hoy falla).
4. Con el motor girando, desconectar el cable → **se detiene en <1 s** y no rearranca.
5. `E:BRAKE_NOW:0` → se detiene. **Esperar 10 s: no debe volver a arrancar solo.**
6. `C:SET_SPEED:-1` → el motor no gira. `C:SET_STEER:-30` → el servo va a la izquierda (50), no a la derecha.

### Fase 1 — Antes de integrar con la Jetson

| # | Hallazgo | Archivo | Motivo |
|---|---|---|---|
| 8 | **A-11** UART1 en GPIO 9/10 | `hardware.h:37-38` | Pines del flash SPI. Muy probablemente la razón por la que el enlace no funciona. Bloquea toda la integración. |
| 9 | **A-6** Fragmentación del parser | `link_rx_task.cpp:66-78` | A 921 600 baudios se pierden comandos de forma no determinista. |
| 10 | **A-3** Watchdog 120 ms vs TTL 200 ms | `hardware.h:50`, `link_rx_task.cpp:128` | El mínimo de 5 Hz que documentamos dispara FAULT continuamente. Hay que fijar los números **antes** de que el equipo Brain calibre su tasa de envío. |
| 11 | **A-9** `M:SYS_MODE:AUTO` → MANUAL | `link_rx_task.cpp:158-164` | Falla silenciosa que deja el sistema sin watchdog creyendo lo contrario. |
| 12 | **A-12** `xTaskCreate` sin verificar | `main.cpp` (8 sitios) | El sistema anuncia `SYSTEM_READY` con tareas de seguridad posiblemente ausentes. |
| 13 | **A-5** Carrera de `tx_queue` | `link_tx_task.cpp:29` | Se pierden los eventos de estado iniciales. |
| 14 | **A-10** Credenciales versionadas | `include/wifi_data.h` | Independiente del resto; hacerlo ya. Requiere además rotar la clave. |

### Fase 2 — Corrección de la documentación (en paralelo con la Fase 1)

| # | Hallazgo | Qué corregir |
|---|---|---|
| 15 | **A-13 / bug #3** | `SERVO_RIGHT` = **160**, no 135. `SERVO_RANGE` correcto = **110**, semi-rango **55**. La fuente de verdad es `include/hardware.h:27-29`. |
| 16 | **M-10** | Eliminar la promesa de "velocidad 100 por defecto": tras C-1, el comportamiento es frenar. |
| 17 | **A-1** | Quitar la garantía de `<1 ms` del `README.md` §2.2 y del protocolo, o implementarla (A-1). |
| 18 | **M-4, M-5, B-5** | Documentar la recuperación de FAULT y el catálogo completo de eventos de salida. |

**Función de conversión correcta** (§1, corrección 1 — el rango real es simétrico, así que es más simple que la del documento):

```python
# Fuente de verdad: include/hardware.h:27-29
SERVO_LEFT   = 50
SERVO_CENTER = 105
SERVO_RIGHT  = 160
SERVO_HALF_RANGE = 55   # == SERVO_CENTER - SERVO_LEFT == SERVO_RIGHT - SERVO_CENTER

def degrees_to_servo(degrees, max_degrees=45):
    """Convierte grados del lane detector (-max_degrees..+max_degrees) a valor de servo.

    Negativo = izquierda, positivo = derecha.
      degrees_to_servo(0)   -> 105  (centro)
      degrees_to_servo(-45) -> 50   (izquierda máxima)
      degrees_to_servo(+45) -> 160  (derecha máxima)
      degrees_to_servo(-20) -> 81
    """
    normalized = max(-1.0, min(1.0, degrees / max_degrees))
    servo = SERVO_CENTER + normalized * SERVO_HALF_RANGE
    return int(round(max(SERVO_LEFT, min(SERVO_RIGHT, servo))))
```

Esta versión es equivalente a la que **ya usa la interfaz web** (`src/webpage.cpp:162-168`), que es correcta. Es decir: el único artefacto equivocado del repositorio es `docs/BRAIN_TEAM_PROTOCOL.md`.

> **Advertencia de coordinación:** si el equipo Brain ya integró contra `SERVO_RIGHT = 135`, sus comandos son válidos pero nunca alcanzan el giro máximo a la derecha (llegan al 55 % del recorrido disponible). Al corregir el documento, **el auto girará más a la derecha que antes con los mismos grados de entrada**. Avisarlo explícitamente y volver a calibrar el lazo de seguimiento de carril.

### Fase 3 — Robustez y calidad (tras validar en banco)

| # | Hallazgo | Motivo |
|---|---|---|
| 19 | **A-2** `pulseIn` bloquea el core 0 | Devuelve el 60 % de la CPU del core 0 al lazo de control. Empezar por la mitigación de una línea (timeout a 5000 µs). |
| 20 | **A-1** Emergencia con `ulTaskNotifyTake` bloqueante | Latencia de emergencia real de `<1 ms`. |
| 21 | **A-4** `volatile` + snapshot atómico | Elimina la dependencia de efectos accidentales del compilador. |
| 22 | **A-7** Marcha atrás rota | Requiere decidir antes la semántica del signo de `SET_SPEED` (cambio de protocolo). |
| 23 | **M-2, M-3** Telemetría entrelazada y `flush()` | Necesario para que el dashboard y `telemetry_monitor.py` sean fiables. |
| 24 | **M-8, M-9, M-7** LEDC, strapping, versión de plataforma | Requieren verificación en hardware; agrupar en una sesión de banco con osciloscopio. |
| 25 | **M-5, M-6, M-11, M-12, M-13, M-14** | Robustez general. |
| 26 | **B-1 … B-7** | Limpieza. Hacer de una sola vez al final, en un commit separado. |

### Fase 4 — Mejora arquitectónica ya identificada por el equipo

`docs/v3.0.0/TODO.md` propone reemplazar el *polling* de mailboxes por notificaciones de FreeRTOS en todas las tareas. Es la evolución correcta y encaja de forma natural con A-1 (que introduce ese mismo patrón en `MotorTask`). Recomendación: **hacer A-1 primero como piloto** en `MotorTask`, validarlo en banco, y luego extender a `SteerTask` y `LightsTask` siguiendo el TODO.

---

## 7. Tabla resumen

| Severidad | Cantidad | Identificadores |
|---|---|---|
| **Crítico** | 5 | C-1 … C-5 |
| **Alto** | 13 | A-1 … A-13 |
| **Medio** | 14 | M-1 … M-14 |
| **Bajo / estilo** | 7 | B-1 … B-7 |
| **Total** | **39** | |

**Verificados leyendo el código:** 36 de 39.
**Marcados como sospecha (requieren prueba en hardware o consulta de versión de librería):** 3 — M-6 (`sdkconfig.defaults` inerte), M-8 (conflicto LEDC servo/motor), M-9 (strapping GPIO 12). A-4 y A-11 están confirmados en cuanto al defecto del código, y marcados como sospecha solo respecto de si llegan a manifestarse como fallo observable.
