# Simulador del firmware en la PC

Compila el firmware **real** (`src/*.cpp`, incluido `hardware.cpp`) para la PC, cambiando FreeRTOS, Arduino
y el hardware por stubs (`stubs/`). Las tareas corren como threads, y el puente H, el servo, el E-STOP y el
HC-SR04 se simulan a nivel de pines. El simulador avisa si se invierte el puente H con PWM aplicado, y mide
el tiempo muerto.

Del lado de la Jetson usa el código **real** de AURA-2026: `CommandSender`, el `parse_system_events` del
dashboard y los métodos de dirección del piloto automático, extraídos con `ast` (así no hacen falta Flask,
OpenCV ni la cámara).

```bash
./run_all.sh                       # compila las 3 variantes y corre todo (código ≠ 0 si algo falla)
./build.sh                         # solo compila -> build/fw_sim
SIM_FLAGS="-DULTRASONIC_USE_PULSEIN" ./build.sh "" build/fw_pulsein
python3 fw_regressions.py build/fw_sim build/fw_wrap   # bugs del firmware ya encontrados
python3 jetson_real.py build/fw_sim                   # escenarios de manejo con la Jetson
python3 jetson_regressions.py build/fw_sim            # bugs de la Jetson (señales, freno, reset)
python3 us_tests.py build/fw_sim                      # ultrasonido
python3 jetson_sim.py build/fw_a build/fw_b           # compara dos firmwares lado a lado
```

| Suite | Qué cubre |
|---|---|
| `fw_regressions.py` | Tiempo muerto aunque se pase por freno, freno contra `SET_SPEED` en el mismo ciclo, `DISARM`+`ARM` seguidos, confirmaciones, `CMD_REJECTED`, watchdog en el wrap del tick |
| `jetson_real.py` | Arranque, crucero, giro, cartel de pare, obstáculo, enlace perdido, marcha atrás, estacionamiento, freno, `SYS_STOP`, E-STOP, re-armado |
| `jetson_regressions.py` | Velocidad vieja después de `FAULT`/reinicio, `OBSTACLE_RESET` sin avance, freno que cancela reanudaciones y estacionamiento, obstáculo que aborta el estacionamiento |
| `us_tests.py` | Umbral, latencia, pérdida de ecos al azar, histéresis, evidencia que vence, reset, salud del sensor, marcha atrás |

Requisitos: un compilador de C++17 (`g++`, `clang++` o `pip install ziglang`), Python 3 y el repo
AURA-2026 al lado de este (o `AURA_BRAIN=/ruta/a/AURA-2026/brain`).

## Control del simulador

El binario lee comandos del protocolo por stdin (es el USB). Las líneas que empiezan con `#SIM` controlan el
mundo simulado:

| Línea | Efecto |
|---|---|
| `#SIM dist <cm>` | El sensor ve un objeto a esa distancia (`0` = sin eco: pulso de ~38 ms, como el HC-SR04) |
| `#SIM pattern a,b,c` | Secuencia de lecturas que se repite en ciclo (`0` = sin eco) |
| `#SIM dropout <p>` | Probabilidad de perder el eco en cada lectura (semilla fija: corridas reproducibles) |
| `#SIM noise <cm>` | Ruido uniforme de ±cm |
| `#SIM uslog 1` | Imprime cada lectura cruda (`#US raw=…`) |
| `#SIM estop <0\|1>` | E-STOP físico |
| `#SIM uart <línea>` | Inyecta la línea por el UART en lugar del USB |

La salida tiene prefijos: `USB …` / `UART …` es lo que escribe el firmware, `#HW …` es el estado del
hardware.

## Límites

- No simula las prioridades ni los cores del ESP32: las tareas corren en paralelo de verdad. Por ejemplo,
  no aparece el bloqueo de `pulseIn()` sobre el lazo de control.
- Los tiempos son reales y del orden de milisegundos, con el jitter de la PC.
- No simula la parte eléctrica: niveles de 5 V en el eco, ruido del motor, rebotes reales.
