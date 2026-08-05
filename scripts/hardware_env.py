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
hardware.env de Brain-Aura y convierte en `-D` del compilador las claves que el
propio archivo marque con una linea:

    # firmware: SERVO_LEFT
    BRAIN_SERVO_IZQUIERDA=50

produce `-DSERVO_LEFT=50`. Los `#define` de include/hardware.h estan envueltos
en `#ifndef`, asi que lo que venga de aca gana.

ESTE SCRIPT NO SABE QUE VARIABLES EXISTEN, a proposito. Antes tenia una lista
hardcodeada y eso era una segunda fuente de verdad: agregar una clave a
hardware.env no alcanzaba, habia que acordarse de tocar tambien este archivo, y
si no el firmware seguia compilando con un valor viejo sin avisar nada.

Ahora hardware.env declara sus propios vinculos y aca no hay nada que
mantener.

DONDE BUSCA EL ARCHIVO, en orden:
    1. $BRAIN_HARDWARE_ENV
    2. ../Brain-Aura/hardware.env   (el layout normal en la Jetson)
    3. ./hardware.env.local         (copia de respaldo de este repo)

Si no encuentra ninguno avisa y compila con los valores de hardware.h. El
firmware NUNCA deja de compilar por esto.
"""

import os

Import("env")  # noqa: F821  (lo inyecta PlatformIO)

# Marca con la que hardware.env declara que la clave SIGUIENTE va al firmware.
DIRECTIVA = "# firmware:"


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
    """Devuelve [(define, valor, clave)] de las claves marcadas para el firmware.

    Una directiva aplica a la PRIMERA asignacion que venga despues; si en el
    medio hay comentarios o lineas en blanco no pasa nada, pero si hay otra
    directiva, la anterior se descarta (quedo colgada sin asignacion).
    """
    inyectar = []
    pendiente = None

    with open(ruta, "r") as f:
        for nro, linea in enumerate(f, 1):
            linea = linea.strip()
            if not linea:
                continue

            if linea.startswith(DIRECTIVA):
                if pendiente is not None:
                    print("[hardware_env] AVISO: la directiva '%s' de la linea %d "
                          "no tenia ninguna variable debajo." % (pendiente[0], pendiente[1]))
                nombre = linea[len(DIRECTIVA):].strip()
                pendiente = (nombre, nro) if nombre else None
                continue

            if linea.startswith("#") or "=" not in linea:
                continue

            clave, _, valor = linea.partition("=")
            clave = clave.strip()
            valor = valor.strip().strip('"').strip("'")

            if pendiente is not None:
                define = pendiente[0]
                pendiente = None
                if valor:  # una clave vacia significa "usa el defecto del firmware"
                    inyectar.append((define, valor, clave))
                else:
                    print("[hardware_env] %s esta vacia: %s se queda con el "
                          "valor de hardware.h" % (clave, define))

    if pendiente is not None:
        print("[hardware_env] AVISO: la directiva '%s' de la linea %d no tenia "
              "ninguna variable debajo." % (pendiente[0], pendiente[1]))

    return inyectar


ruta = next((r for r in _candidatos() if os.path.isfile(r)), None)

if ruta is None:
    print("[hardware_env] AVISO: no se encontro hardware.env. Se compila con "
          "los valores de include/hardware.h, que pueden no coincidir con los "
          "que usa Brain-Aura.")
else:
    inyectar = _leer(ruta)

    # Un -DNOMBRE solo tiene efecto si el #define de hardware.h esta envuelto en
    # #ifndef. Si no, el header redefine el macro y GANA, y hardware.env queda
    # ignorado sin que nada falle: el firmware sale con un valor distinto del
    # que cree Brain-Aura. Es exactamente el problema que este script existe
    # para evitar, asi que se avisa fuerte.
    try:
        with open(os.path.join(env.subst("$PROJECT_DIR"), "include", "hardware.h")) as f:  # noqa: F821
            header = f.read()
    except (IOError, OSError):
        header = None

    if header is not None:
        for define, _, clave in inyectar:
            if ("#ifndef %s" % define) not in header:
                print("[hardware_env] AVISO: %s (%s) NO esta envuelto en "
                      "'#ifndef %s' en include/hardware.h, asi que el header lo "
                      "redefine y este valor NO se aplica. Envolvelo o el "
                      "firmware va a usar otro numero que Brain-Aura."
                      % (define, clave, define))

    if inyectar:
        env.Append(BUILD_FLAGS=["-D%s=%s" % (d, v) for d, v, _ in inyectar])  # noqa: F821
        print("[hardware_env] %s -> %d valores compartidos:" % (ruta, len(inyectar)))
        for define, valor, clave in inyectar:
            print("[hardware_env]   -D%s=%s   (%s)" % (define, valor, clave))
    else:
        print("[hardware_env] AVISO: %s no marca ninguna clave con '%s'. El "
              "firmware compila con los valores de hardware.h, que pueden no "
              "coincidir con los que usa Brain-Aura." % (ruta, DIRECTIVA))
