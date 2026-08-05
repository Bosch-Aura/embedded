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


def _fuentes(raiz):
    """{ruta relativa: contenido} de todos los .h y .cpp del proyecto.

    Se mira el arbol entero, no solo hardware.h: hay #define que dependen del
    hardware y viven en las tareas (el tiempo muerto del puente H, por ejemplo).
    """
    textos = {}
    for carpeta in ("include", "src"):
        base = os.path.join(raiz, carpeta)
        for dirpath, _, ficheros in os.walk(base):
            for nombre in ficheros:
                if not nombre.endswith((".h", ".hpp", ".c", ".cpp")):
                    continue
                completa = os.path.join(dirpath, nombre)
                try:
                    with open(completa, "r") as f:
                        textos[os.path.relpath(completa, raiz)] = f.read()
                except (IOError, OSError, UnicodeDecodeError):
                    pass
    return textos


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

    # Un -DNOMBRE solo tiene efecto si el #define correspondiente esta envuelto
    # en #ifndef. Si no, el codigo redefine el macro y GANA, y hardware.env
    # queda ignorado sin que nada falle: el firmware sale con un valor distinto
    # del que cree Brain-Aura.
    #
    # Es exactamente el desajuste que este script existe para evitar, y es
    # invisible, asi que CORTA la compilacion. Mejor no compilar que flashear un
    # auto cuyos numeros no coinciden con los del que lo maneja.
    fuentes = _fuentes(env.subst("$PROJECT_DIR"))  # noqa: F821
    rotos = []
    for define, _, clave in inyectar:
        donde_define = [r for r, t in fuentes.items() if ("#define %s " % define) in t]
        donde_guarda = [r for r, t in fuentes.items() if ("#ifndef %s" % define) in t]
        if donde_define and not donde_guarda:
            rotos.append((define, clave, donde_define[0]))

    if rotos:
        print("")
        print("[hardware_env] ERROR: hay claves marcadas para el firmware cuyo "
              "#define no esta envuelto en #ifndef.")
        print("[hardware_env] El -D no les gana, asi que hardware.env quedaria "
              "IGNORADO en silencio para estos valores:")
        for define, clave, donde in rotos:
            print("[hardware_env]")
            print("[hardware_env]   %s  (viene de %s)" % (define, clave))
            print("[hardware_env]   definido sin envolver en %s" % donde)
            print("[hardware_env]   arreglo:  #ifndef %s" % define)
            print("[hardware_env]             #define %s <valor>" % define)
            print("[hardware_env]             #endif")
        print("")
        Exit(1)  # noqa: F821

    if inyectar:
        env.Append(BUILD_FLAGS=["-D%s=%s" % (d, v) for d, v, _ in inyectar])  # noqa: F821
        print("[hardware_env] %s -> %d valores compartidos:" % (ruta, len(inyectar)))
        for define, valor, clave in inyectar:
            print("[hardware_env]   -D%s=%s   (%s)" % (define, valor, clave))
    else:
        print("[hardware_env] AVISO: %s no marca ninguna clave con '%s'. El "
              "firmware compila con los valores de hardware.h, que pueden no "
              "coincidir con los que usa Brain-Aura." % (ruta, DIRECTIVA))
