# Firmware del ESP32 (Vehicle Control Unit)

**Plataforma:** ESP32 DevKit v1 · Arduino + FreeRTOS · PlatformIO

El ESP32 es la capa de bajo nivel de un auto autónomo a escala. Recibe órdenes de la Jetson por serie y:

- mantiene una **velocidad crucero** (PWM latcheado) en el sentido pedido;
- mueve el **servo de dirección** al ángulo pedido;
- obedece **stop / start** (`M:SYS_STOP` / `M:SYS_RESUME`) y un freno de emergencia;
- **frena solo ante un obstáculo** detectado por el ultrasonido (HC-SR04) y avisa a la Jetson;
- **detiene todo** si se pierde el enlace con la Jetson (watchdog de 300 ms) o si se acciona el E-STOP.

El protocolo completo está en [`docs/BRAIN_TEAM_PROTOCOL.md`](docs/BRAIN_TEAM_PROTOCOL.md).

## Compilar y flashear

```bash
pio run -e esp32doit-devkit-v1              # compilar
pio run -e esp32doit-devkit-v1 -t upload    # flashear
./flashear.sh                               # o el script, que también detecta el puerto
```

El entorno `esp32doit-devkit-v1-debug` espera 3 s al arrancar para que se conecte el monitor serie. No se
usa en el auto.

El entorno `esp32doit-devkit-v1-usdebug` imprime por USB cada lectura del ultrasónico y cada decisión de
frenar o liberar (ver [Prueba de banco del freno](#prueba-de-banco-del-freno-por-ultrasónico)). Tampoco se
usa en el auto.

**Velocidad mínima: `C:SET_SPEED:130`.** Por debajo el motor zumba pero no arranca.

## Arquitectura

Cuatro tareas FreeRTOS. Cada recurso de hardware tiene **un solo dueño**, y los estados los escribe una
sola tarea.

| Tarea | Core | Prioridad | Período | Responsabilidad |
|---|---|---|---|---|
| `Ultrasonic` (`ultrasonic_task.cpp`) | 0 | 5 | 50 ms + lectura | Lee el HC-SR04 y decide si hay obstáculo. Avisa al supervisor. Mientras espera el eco duerme: el pulso se mide con una interrupción (`hardware.cpp`). |
| `Control` (`control_task.cpp`) | 0 | 4 | 10 ms, o en el acto si la despiertan | **Única dueña del puente H y del servo.** Aplica velocidad, sentido y ángulo según el estado. |
| `LinkRx` (`link_rx_task.cpp`) | 1 | 4 | 10 ms | Arma líneas desde USB y UART, interpreta los comandos y alimenta el watchdog. |
| `Supervisor` (`supervisor_task.cpp`) | 1 | 2 | 20 ms | **Único que escribe el estado** (`DISARMED/RUNNING/STOPPED/FAULT`). Watchdog y E-STOP. |

```
 Jetson ──USB/UART──▶ LinkRx ──control_set_*()──────────────▶ Control ──▶ puente H, servo
                         │                                      ▲  ▲
                         ├─supervisor_request_arm/disarm()─▶ Supervisor ──estado──┘  │
                         ├─supervisor_latch/release_stop()─────┘   ▲                 │
                         └─ultrasonic_request_reset()──▶ Ultrasonic ─set_obstacle()──┘
 Todas las tareas ──event_*()/log_*()──▶ serial_out ──▶ USB (+ UART para los EVENT:)
```

### Cómo se comunican

- **Comandos hacia `Control`**: un struct protegido por un spinlock (`portMUX`). Cada comando lleva un
  número de secuencia y se aplica **exactamente una vez** en el ciclo siguiente. El freno usa el mismo canal que
  la velocidad (velocidad 0), así que decide el orden de llegada.
- **Despertar inmediato**: `Control` duerme con `ulTaskNotifyTake(…, 10 ms)`. Un freno, un obstáculo, un
  `SYS_STOP` o un cambio de estado le mandan una notificación (`control_wake()`), así que reacciona en el
  acto en lugar de esperar al próximo ciclo.
- **Estado**: `volatile`, lo escribe solo `Supervisor`. `SYS_ARM` y `SYS_DISARM` llegan por una cola chica y
  se procesan en orden. Al pasar a `DISARMED` o `FAULT` se borra la velocidad explícitamente, por si se vuelve a
  armar antes de que `Control` llegue a ver el cambio.
- **Paradas**: `obstacle_active` lo escribe `Ultrasonic` y `stop_latched` lo escribe `LinkRx`. `Control` los
  lee directamente para frenar sin esperar al supervisor.
- **Salida serie**: `serial_out` arma cada línea completa y la escribe bajo un mutex, así nunca salen dos
  líneas pegadas. `log_*` sale solo por USB; `event_*` sale por USB y UART. Los buffers de transmisión son
  de 1 KB, para que escribir un evento no frene el lazo de control.

### Reglas de seguridad (no romperlas)

1. **Fail-safe en `Control`**: si no está armado, si hay `SYS_STOP` o si se intenta avanzar contra un
   obstáculo, el auto **frena**. Con `DISARMED`, `FAULT` o `SYS_STOP` se borra la velocidad latcheada y nunca
   vuelve a arrancar solo. Con un obstáculo la velocidad se guarda y el auto **retoma solo** al despejarse
   (`OBSTACLE_HOLD` / `OBSTACLE_RESUMED`); un `OBSTACLE_RESET`, un freno o cualquier otra parada la borran.
2. **Nunca se invierte el puente H con PWM aplicado**: primero se corta el PWM, se esperan 60 ms
   (`MOTOR_DIRECTION_DEADTIME_MS`) y recién ahí se cambian IN3/IN4. Los 60 ms se cuentan desde la última vez
   que el motor anduvo en el otro sentido, **aunque en el medio haya frenado**: frenado no quiere decir quieto.
   La espera no bloquea el lazo.
3. **Los valores se saturan en el tipo con signo**, antes de convertirlos (si no, `-1` termina siendo 255).
4. **Watchdog**: la antigüedad del último comando se calcula con el reloj leído *después* del heartbeat,
   como resta sin signo (así también es correcta cuando el contador de ticks da la vuelta). Solo alimentan el
   watchdog los comandos reconocidos.
5. **Ultrasonido**: para frenar solo cuentan las lecturas válidas. Para soltar el freno también vale "sin
   eco" sostenido 2 s (el sensor disparó y no volvió nada), nunca un sensor que no responde (ver
   `ultrasonic_task.cpp`).
6. **`setup()` pone el hardware en estado seguro antes que cualquier otra cosa** y, si alguna tarea no
   se puede crear, reinicia la placa en lugar de arrancar sin watchdog.

## Configuración

Los valores de hardware (pines, topes del servo, umbral del ultrasonido, watchdog) están en
`include/hardware.h`, envueltos en `#ifndef`. Se pueden sobrescribir con `-D` o desde
`Brain-Aura/hardware.env` (ver `scripts/hardware_env.py` y la última sección del protocolo).

Flags de compilación útiles: `-DBOOT_ARMED` (arranca en `RUNNING`), `-DULTRASONIC_FACING_FORWARD=0` (el
sensor está atrás), `-DULTRASONIC_USE_PULSEIN` (mide el eco con `pulseIn()` en lugar de la interrupción),
`-DMOTOR_DIRECTION_DEADTIME_MS=…`, `-DULTRASONIC_DEBOUNCE_COUNT=…`.

## Pruebas sin el auto

`test/sim/` compila el firmware real para la PC, con FreeRTOS, Arduino y los pines simulados. Lo maneja
con el código real de AURA-2026 (`CommandSender`, el dashboard, las estrategias de señales) e incluye
escenarios de manejo, estacionamiento, watchdog (también en el wrap del tick), E-STOP, regresiones de bugs
ya encontrados y ultrasonido con pérdida de ecos, con los dos drivers de eco. `./test/sim/run_all.sh` corre
todo. Ver [`test/sim/README.md`](test/sim/README.md).

`test/python/test_uart_simulator.py` es una consola interactiva para mandarle comandos a la placa real.

### Prueba de banco del freno por ultrasónico

Comprueba en la placa real que el sensor mide bien y que el auto frena cuando tiene que frenar, sin la
Jetson. **Las ruedas giran: levantá el auto.**

```bash
./flashear.sh --env esp32doit-devkit-v1-usdebug   # firmware con logs del ultrasónico
python3 scripts/prueba_freno.py                   # arma, avanza a 150 y muestra los logs
```

El script hace de Brain por el USB como la Jetson: `SYS_ARM`, `SET_DIR:1`, manda `SET_SPEED` 3 veces y
después solo `PING` a 10 Hz (el latido que alimenta el watchdog). Imprime todo lo que dice la placa con hora y lo guarda en `prueba_freno_<fecha>.txt`. Al acercar
un objeto a menos de 30 cm tiene que aparecer:

```
[US] >>> FRENO: obstaculo a 25 cm (umbral 30)
EVENT:OBSTACLE_DETECTED:25
[Control] >>> MOTOR FRENADO por obstaculo (velocidad 170 -> 0)
```

- `--velocidad N`: PWM a mandar. **El mínimo para que se mueva es 130**; con menos el motor zumba y no gira.
- `--veces N`: cuántas veces se manda `SET_SPEED` al principio (default 3). Al sacar el obstáculo el auto
  tiene que **retomar solo** (`EVENT:OBSTACLE_RESUMED:<pwm>`), sin que el script reenvíe nada.
- Ctrl+C manda `SET_SPEED:0` y `SYS_DISARM` antes de salir.

Cerrá cualquier monitor serie antes: el puerto lo tiene que tener solo el script. Para ver solo los logs,
sin mover nada: `pio device monitor --port /dev/ttyCH341USB0 --baud 115200 --rts 0 --dtr 0`. Después de la
prueba, volvé a flashear el firmware normal con `./flashear.sh`.

---

## Licencia

Este proyecto se distribuye bajo la **Licencia MIT** — ver [LICENSE](LICENSE).

Podés usar, modificar y redistribuir este firmware, incluso con fines comerciales,
siempre que conserves el aviso de copyright. Se entrega sin garantías.

### Dependencias y sus licencias

El código de `src/` e `include/` es propio. Las dependencias externas conservan sus
propias licencias:

| Dependencia | Licencia | Alcance |
|---|---|---|
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | LGPL-2.1 | Framework, se enlaza pero no se redistribuye acá |
| [ESP32Servo](https://github.com/madhephaestus/ESP32Servo) | ver su repositorio | Librería de control de servo |

> **Nota:** La LGPL impone obligaciones al distribuir **binarios** enlazados.
> Este repositorio distribuye únicamente código fuente, por lo que no se ven
> afectadas. Si publicás un `.bin` compilado, revisá los términos de la LGPL-2.1.
