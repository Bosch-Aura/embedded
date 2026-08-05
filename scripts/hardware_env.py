"""Comparte hardware.env entre Brain-Aura y el firmware.

PROBLEMA QUE RESUELVE

Los topes del servo, el watchdog y el baudrate tienen que coincidir SI O SI
entre los dos lados del enlace. Estaban escritos dos veces -- en
`Brain-Aura/hardware.env` y en `embedded/include/hardware.h` -- y nada obligaba
a que dijeran lo mismo. Ya paso una vez: la documentacion decia que el tope
derecho del servo era 135 y el firmware usaba 160, y el auto giraba a la
derecha mucho mas de lo que el otro lado creia.

COMO FUNCIONA

Es un extra_script de PlatformIO que corre ANTES de compilar. Lee el
hardware.env de Brain-Aura y convierte las claves compartidas en `-D` del
compilador. Los `#define` de include/hardware.h estan envueltos en `#ifndef`,
asi que lo que venga de aca gana.

Resultado: hardware.env es la unica fuente de verdad, y para que el firmware la
tome alcanza con recompilar.

DONDE BUSCA EL ARCHIVO, en orden:
    1. $BRAIN_HARDWARE_ENV
    2. ../Brain-Aura/hardware.env   (el layout normal en la Jetson)
    3. ./hardware.env.local         (copia de respaldo de este repo)

Si no encuentra ninguno avisa y compila con los valores de hardware.h. El
firmware NUNCA deja de compilar por esto.
"""

import os

Import("env")  # noqa: F821  (lo inyecta PlatformIO)

# Clave de hardware.env -> nombre del #define en include/hardware.h.
# SOLO van las que los DOS lados tienen que ver igual. Los numeros de pin no
# estan a proposito: los usa unicamente el firmware, compartirlos agregaria
# acoplamiento sin que nadie del otro lado los mire.
CLAVES_COMPARTIDAS = {
    "BRAIN_SERVO_IZQUIERDA": "SERVO_LEFT",
    "BRAIN_SERVO_CENTRO": "SERVO_CENTER",
    "BRAIN_SERVO_DERECHA": "SERVO_RIGHT",
    "BRAIN_DUTY_MAX": "MOTOR_SPEED_MAX",
    "BRAIN_WATCHDOG_AUTO_MS": "WATCHDOG_TIMEOUT_AUTO_MS",
    "BRAIN_WATCHDOG_MANUAL_MS": "WATCHDOG_TIMEOUT_MANUAL_MS",
    "BRAIN_ULTRASONIC_UMBRAL_CM": "ULTRASONIC_OBSTACLE_THRESHOLD_CM",
}


def _candidatos():
    # SCons ejecuta este script sin __file__, asi que la raiz del proyecto se
    # pide por la variable que expone PlatformIO.
    aqui = env.subst("$PROJECT_DIR")  # noqa: F821
    desde_entorno = os.environ.get("BRAIN_HARDWARE_ENV")
    if desde_entorno:
        yield desde_entorno
    yield os.path.join(os.path.dirname(aqui), "Brain-Aura", "hardware.env")
    yield os.path.join(aqui, "hardware.env.local")


def _leer(ruta):
    valores = {}
    with open(ruta, "r") as f:
        for linea in f:
            linea = linea.strip()
            if not linea or linea.startswith("#") or "=" not in linea:
                continue
            clave, _, valor = linea.partition("=")
            valor = valor.strip().strip('"').strip("'")
            if valor:  # una clave vacia significa "usa el defecto"
                valores[clave.strip()] = valor
    return valores


ruta = next((r for r in _candidatos() if os.path.isfile(r)), None)

if ruta is None:
    print("[hardware_env] AVISO: no se encontro hardware.env. Se compila con "
          "los valores de include/hardware.h, que pueden no coincidir con los "
          "que usa Brain-Aura.")
else:
    valores = _leer(ruta)
    flags = []
    for clave, define in CLAVES_COMPARTIDAS.items():
        if clave in valores:
            flags.append("-D%s=%s" % (define, valores[clave]))

    if flags:
        env.Append(CPPDEFINES=[])  # noqa: F821
        env.Append(BUILD_FLAGS=flags)  # noqa: F821
        print("[hardware_env] %s -> %d valores compartidos:" % (ruta, len(flags)))
        for f in flags:
            print("[hardware_env]   %s" % f)
    else:
        print("[hardware_env] %s no define ninguna clave compartida." % ruta)
