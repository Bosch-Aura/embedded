# Quick Testing Guide

> **Actualizado.** Esta guía decía que el UART externo iba en GPIO 9/10, usaba flags del
> simulador que no existen (`-p`, `--demo`) y apuntaba a archivos que no están en el repo
> (`test/test_commands.txt`, `docs/TESTING_WITHOUT_JETSON.md`). Lo de abajo refleja el firmware
> actual. La referencia del protocolo es `docs/BRAIN_TEAM_PROTOCOL.md`.

## Testing Without Jetson

### Opción 1: simulador en Python (la más fácil)

No hace falta adaptador: el firmware también lee comandos por el **USB** (115200 baud).

```bash
# Dependencias
pip install -r test/python/requirements.txt

# Simulador interactivo (el puerto es posicional; si se omite, lo pregunta)
python3 test/python/test_uart_simulator.py /dev/ttyUSB0          # PC
python3 test/python/test_uart_simulator.py /dev/ttyCH341USB0     # Jetson (driver WCH)

# Dentro del simulador, "demo" corre una secuencia de zigzag
```

`test/test_uart_simulator.py` (el de la raíz de `test/`) es una versión vieja que manda
`CONTROL:...`/`LIGHTS:...` y usa 921600 por defecto: usá el de `test/python/`.

El simulador **no manda `M:PING:0`** por su cuenta: escribiendo a mano, al segundo sin comandos
el watchdog de MANUAL lleva el sistema a `FAULT`. Ver `test/QUICK_UART_TEST.md`.

### Opción 2: terminal serie

- **Por USB:** 115200 baud, el mismo puerto que usa `pio device monitor`.
- **Por el UART externo:** adaptador en **GPIO 17 (TX)** y **GPIO 16 (RX)** del ESP32, a **921600 baud**.
  Por ahí solo vuelven `EVENT:STATE_CHANGED` y `EVENT:MODE_CHANGED`; el resto de la salida sale por USB.

### Opción 3: interfaz web

1. Conectarse al Wi-Fi `RC-Car-ESP32` (abierto)
2. Abrir `http://192.168.4.1`
3. Usar ARMAR / DESARMAR / E-STOP, el slider de velocidad y el volante

## Quick Test Commands

```
M:SYS_MODE:1        # Pasar a AUTO
M:SYS_ARM:0         # Armar (obligatorio: arranca desarmado)
C:SET_SPEED:150     # Velocidad (latching: queda sostenida; por debajo de ~140 no arranca)
C:SET_STEER:105     # Dirección (50 izquierda, 105 centro, 160 derecha)
M:PING:0            # Latido: hay que mandarlo (o cualquier comando) a 10 Hz
E:BRAKE_NOW:0       # Freno de emergencia (bloqueo de 5 s)
M:SYS_DISARM:0      # Desarmar
```

Ver `test/TESTING_WITHOUT_JETSON.md` para la guía completa.
