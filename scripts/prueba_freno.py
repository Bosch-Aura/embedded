#!/usr/bin/env python3
"""Prueba de banco del freno y la reanudacion por ultrasonico, sin la Jetson/Brain.

Hace de "Brain" por el USB de la ESP32, como lo haria la Jetson de verdad:
    1. M:SYS_ARM:0               arma el auto
    2. C:SET_DIR:1               hacia adelante (hacia el sensor)
    3. C:SET_SPEED:<v> x3        manda la velocidad 3 veces y NUNCA MAS
    4. M:PING:0 a 10 Hz          solo el latido: sin el, a los 300 ms el
                                 watchdog pone el auto en FAULT
Y al mismo tiempo imprime todo lo que dice el firmware, con hora, y lo guarda
en un archivo.

Caso a probar:
    - El auto arranca a <v>.
    - Acercale un objeto al sensor: frena solo
          [US] >>> FRENO ...   EVENT:OBSTACLE_DETECTED   EVENT:OBSTACLE_HOLD:<v>
    - Saca el objeto (aunque no quede nada adelante): a los ~2 s retoma solo
          EVENT:OBSTACLE_CLEARED   EVENT:OBSTACLE_RESUMED:<v>
      sin que este script mande ninguna velocidad nueva.

Ctrl+C: manda C:SET_SPEED:0 y M:SYS_DISARM:0 antes de salir.

ATENCION: las ruedas van a girar. Probalo con el auto levantado.

Uso:
    python3 scripts/prueba_freno.py [--velocidad 170] [--veces 3]
                                    [--puerto /dev/ttyCH341USB0] [--log archivo]
Para ver las lineas [US] y [Control] hay que flashear el entorno de debug:
    ./flashear.sh --env esp32doit-devkit-v1-usdebug
El minimo para que el motor se mueva es 130: con menos zumba y no gira.
Cerra cualquier monitor serie antes: el puerto lo tiene que tener solo este script.
"""

import argparse
import datetime
import sys
import time

import serial

PERIODO_S = 0.1  # 10 Hz, lo que espera el firmware (LINK_EXPECTED_PERIOD_MS)

ROJO, VERDE, AMARILLO, OFF = "\033[31m", "\033[32m", "\033[33m", "\033[0m"


def color_de(linea):
    if ("FRENO" in linea or "FRENADO" in linea or "OBSTACLE_DETECTED" in linea
            or "OBSTACLE_HOLD" in linea or "FAULT" in linea):
        return ROJO
    if ("LIBERADO" in linea or "liberado" in linea or "OBSTACLE_CLEARED" in linea
            or "OBSTACLE_RESUMED" in linea):
        return VERDE
    if linea.startswith("EVENT:") or linea.startswith(">>"):
        return AMARILLO
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--puerto", default="/dev/ttyCH341USB0")
    # Por debajo de 130 el motor zumba pero no arranca (docs/BRAIN_TEAM_PROTOCOL.md).
    ap.add_argument("--velocidad", type=int, default=170, help="PWM 0-255 (default 170)")
    ap.add_argument("--veces", type=int, default=3,
                    help="cuantas veces se manda C:SET_SPEED al principio (default 3)")
    ap.add_argument("--log", default=None,
                    help="archivo de salida (default prueba_freno_<fecha>.txt)")
    args = ap.parse_args()

    log_path = args.log or datetime.datetime.now().strftime("prueba_freno_%Y%m%d_%H%M%S.txt")
    color = sys.stdout.isatty()

    s = serial.Serial()
    s.port = args.puerto
    s.baudrate = 115200
    s.timeout = 0
    # Con el CH340, RTS activo deja la ESP32 retenida en reset.
    s.rts = False
    s.dtr = False
    s.open()

    log = open(log_path, "w", buffering=1)

    def mostrar(linea):
        t = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        log.write(f"{t} {linea}\n")
        c = color_de(linea) if color else ""
        print(f"{t} {c}{linea}{OFF if c else ''}")

    def enviar(cmd):
        s.write((cmd + "\n").encode())
        if not cmd.startswith("M:PING"):  # el latido no se muestra
            mostrar(f">> {cmd}")

    mostrar(f"== puerto {args.puerto}, velocidad {args.velocidad} x{args.veces} y despues solo PING, "
            f"log {log_path}")
    mostrar("== ATENCION: las ruedas van a girar. Ctrl+C para frenar y desarmar.")

    buf = b""
    velocidades_enviadas = 0
    proximo = time.monotonic()
    try:
        time.sleep(0.3)
        s.reset_input_buffer()
        enviar("M:SYS_ARM:0")
        enviar("C:SET_DIR:1")
        while True:
            ahora = time.monotonic()
            if ahora >= proximo:
                proximo = ahora + PERIODO_S
                if velocidades_enviadas < args.veces:
                    enviar(f"C:SET_SPEED:{args.velocidad}")
                    velocidades_enviadas += 1
                    if velocidades_enviadas == args.veces:
                        mostrar("== listo: no mando mas velocidad, solo PING (latido)")
                else:
                    enviar("M:PING:0")

            buf += s.read(4096)
            while b"\n" in buf:
                linea, buf = buf.split(b"\n", 1)
                linea = linea.decode(errors="replace").strip()
                if linea:
                    mostrar(linea)
            time.sleep(0.005)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            enviar("C:SET_SPEED:0")
            enviar("M:SYS_DISARM:0")
            time.sleep(0.2)
            for linea in s.read(4096).decode(errors="replace").splitlines():
                if linea.strip():
                    mostrar(linea.strip())
        finally:
            s.close()
            log.close()
            print(f"\nLog guardado en {log_path}")


if __name__ == "__main__":
    main()
