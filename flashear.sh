#!/usr/bin/env bash
#
# flashear.sh - Compila y flashea el firmware ESP32 de este proyecto (PlatformIO).
#
# CONTEXTO IMPORTANTE DE ESTA JETSON:
#   En esta maquina esta instalado el driver VENDOR de WCH (pisa el ch341.ko del kernel).
#   Eso tiene dos consecuencias directas:
#     1) El nodo del dispositivo es /dev/ttyCH341USB0, NO el estandar /dev/ttyUSB0.
#     2) El driver registra el puerto en el bus "usb" en vez de "usb-serial", asi que
#        serial.tools.list_ports.comports() de pyserial devuelve una lista VACIA.
#        Como esptool.py (que PlatformIO usa por debajo) se apoya en pyserial para
#        autodetectar el puerto, LA AUTODETECCION SIEMPRE FALLA.
#   Por eso este script NUNCA confia en la autodeteccion: siempre pasa el puerto
#   de forma explicita con --upload-port / --port. Ese es el punto central del script.
#
# Uso:  ./flashear.sh [-m|--monitor] [-c|--clean] [-p|--port <dev>] [-h|--help]
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuracion basica
# ---------------------------------------------------------------------------

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INI_FILE="${PROJECT_DIR}/platformio.ini"

# Puertos candidatos, en orden de probabilidad REAL en esta Jetson.
PUERTOS_CANDIDATOS=(
    "/dev/ttyCH341USB0"
    /dev/ttyCH341USB*
    /dev/ttyUSB*
    /dev/ttyACM*
)

# ---------------------------------------------------------------------------
# Colores y helpers de salida
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    C_ROJO=$'\033[0;31m'; C_VERDE=$'\033[0;32m'; C_AMARILLO=$'\033[0;33m'
    C_AZUL=$'\033[0;36m'; C_NEGRITA=$'\033[1m'; C_OFF=$'\033[0m'
else
    C_ROJO=''; C_VERDE=''; C_AMARILLO=''; C_AZUL=''; C_NEGRITA=''; C_OFF=''
fi

info()  { printf '%s[INFO]%s  %s\n'  "$C_AZUL"     "$C_OFF" "$*"; }
ok()    { printf '%s[OK]%s    %s\n'  "$C_VERDE"    "$C_OFF" "$*"; }
aviso() { printf '%s[AVISO]%s %s\n'  "$C_AMARILLO" "$C_OFF" "$*" >&2; }
error() { printf '%s[ERROR]%s %s\n'  "$C_ROJO"     "$C_OFF" "$*" >&2; }

# Sale con mensaje de error.
morir() {
    error "$1"
    shift || true
    for linea in "$@"; do
        printf '        %s\n' "$linea" >&2
    done
    exit 1
}

titulo() {
    printf '\n%s=== %s ===%s\n' "$C_NEGRITA" "$*" "$C_OFF"
}

# ---------------------------------------------------------------------------
# Ayuda
# ---------------------------------------------------------------------------

mostrar_ayuda() {
    cat <<'AYUDA'
flashear.sh - Compila y flashea el firmware ESP32 (PlatformIO / Arduino)

USO
    ./flashear.sh [OPCIONES]

OPCIONES
    -m, --monitor        Abre el monitor serie despues de flashear (115200 baudios).
                         Se sale con Ctrl+C.
    -c, --clean          Hace un build limpio: borra los objetos previos antes de
                         compilar (equivale a 'pio run -t clean').
    -p, --port <dev>     Fuerza el puerto serie a usar, por ejemplo:
                             ./flashear.sh --port /dev/ttyCH341USB0
                         Tiene prioridad sobre la deteccion automatica.
    -h, --help           Muestra esta ayuda y sale.

VARIABLES DE ENTORNO
    ESP32_PORT           Puerto serie a usar. Equivalente a --port, pero --port
                         tiene mas prioridad. Ejemplo:
                             ESP32_PORT=/dev/ttyCH341USB0 ./flashear.sh

EJEMPLOS
    ./flashear.sh                       Compila y flashea.
    ./flashear.sh -m                    Compila, flashea y abre el monitor serie.
    ./flashear.sh --clean --monitor     Build limpio, flashea y monitorea.
    ./flashear.sh -p /dev/ttyUSB0       Fuerza otro puerto.

NOTA SOBRE EL PUERTO EN ESTA JETSON
    Aca esta instalado el driver vendor de WCH, asi que la ESP32 aparece como
    /dev/ttyCH341USB0 y NO como /dev/ttyUSB0. Ademas pyserial no la lista, por lo
    que la autodeteccion de puerto de esptool/PlatformIO falla siempre. Este script
    detecta el nodo por si mismo y se lo pasa explicitamente a PlatformIO.
AYUDA
}

# ---------------------------------------------------------------------------
# Parseo de argumentos
# ---------------------------------------------------------------------------

ABRIR_MONITOR=0
BUILD_LIMPIO=0
PUERTO_FORZADO="${ESP32_PORT:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--monitor)
            ABRIR_MONITOR=1
            shift
            ;;
        -c|--clean)
            BUILD_LIMPIO=1
            shift
            ;;
        -p|--port)
            [[ $# -ge 2 ]] || morir "La opcion '$1' necesita un argumento." \
                "Ejemplo: ./flashear.sh --port /dev/ttyCH341USB0"
            PUERTO_FORZADO="$2"
            shift 2
            ;;
        --port=*)
            PUERTO_FORZADO="${1#*=}"
            shift
            ;;
        -h|--help)
            mostrar_ayuda
            exit 0
            ;;
        *)
            error "Opcion desconocida: '$1'"
            printf '\n' >&2
            mostrar_ayuda >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# 1. Verificar que estamos en el proyecto correcto
# ---------------------------------------------------------------------------

titulo "Verificando el proyecto"

[[ -f "$INI_FILE" ]] || morir \
    "No encontre 'platformio.ini' en ${PROJECT_DIR}." \
    "Este script tiene que vivir en la raiz del proyecto PlatformIO." \
    "Movelo junto a platformio.ini y volve a ejecutarlo."

# Sacamos el nombre del entorno desde platformio.ini (primer [env:...]).
PIO_ENV="$(sed -n 's/^[[:space:]]*\[env:\([^]]*\)\].*/\1/p' "$INI_FILE" | head -n1)"
[[ -n "$PIO_ENV" ]] || morir \
    "No pude leer ningun entorno '[env:...]' de ${INI_FILE}." \
    "Revisa que platformio.ini no este corrupto."

ok "Proyecto: ${PROJECT_DIR}"
ok "Entorno PlatformIO: ${PIO_ENV}"

# ---------------------------------------------------------------------------
# 2. Verificar que PlatformIO este instalado
# ---------------------------------------------------------------------------

titulo "Verificando PlatformIO"

PIO_BIN=""
for candidato in pio platformio; do
    if command -v "$candidato" >/dev/null 2>&1; then
        PIO_BIN="$(command -v "$candidato")"
        break
    fi
done

# Rutas tipicas de instalacion cuando no esta en el PATH.
if [[ -z "$PIO_BIN" ]]; then
    for ruta in \
        "${HOME}/.platformio/penv/bin/pio" \
        "${HOME}/.platformio/penv/bin/platformio" \
        "${HOME}/.local/bin/pio" \
        "${HOME}/.local/bin/platformio" \
        "/usr/local/bin/pio"
    do
        if [[ -x "$ruta" ]]; then
            PIO_BIN="$ruta"
            aviso "PlatformIO no esta en el PATH, pero lo encontre en: ${PIO_BIN}"
            aviso "Para tenerlo siempre a mano agrega a tu ~/.bashrc:"
            aviso "    export PATH=\"\$PATH:$(dirname "$ruta")\""
            break
        fi
    done
fi

if [[ -z "$PIO_BIN" ]]; then
    error "No encontre PlatformIO instalado en este sistema."
    cat >&2 <<'INSTALL'

        Instalalo vos mismo (NO lo instalo yo automaticamente).
        En esta Jetson (Ubuntu arm64) la forma recomendada es:

            sudo apt-get update
            sudo apt-get install -y python3 python3-pip python3-venv
            python3 -m pip install --user --upgrade platformio

        Despues agrega el binario al PATH y recarga la shell:

            echo 'export PATH="$PATH:$HOME/.local/bin"' >> ~/.bashrc
            source ~/.bashrc

        Verificalo con:

            pio --version

        Alternativa oficial (instalador standalone):

            curl -fsSL -o get-platformio.py \
                https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
            python3 get-platformio.py

        Ojo: en arm64 la toolchain de Xtensa para ESP32 se baja sola la primera vez
        que compiles. Puede tardar varios minutos y necesita internet.

INSTALL
    exit 1
fi

if ! PIO_VERSION="$("$PIO_BIN" --version 2>&1)"; then
    morir "Encontre '${PIO_BIN}' pero falla al ejecutarse." \
        "Salida: ${PIO_VERSION}" \
        "Proba reinstalar PlatformIO: python3 -m pip install --user --force-reinstall platformio"
fi

ok "PlatformIO: ${PIO_VERSION} (${PIO_BIN})"

# ---------------------------------------------------------------------------
# 3. Detectar el puerto serie
# ---------------------------------------------------------------------------

titulo "Detectando el puerto serie"

PUERTO=""

if [[ -n "$PUERTO_FORZADO" ]]; then
    if [[ ! -e "$PUERTO_FORZADO" ]]; then
        morir "El puerto que forzaste no existe: ${PUERTO_FORZADO}" \
            "Revisa el nombre. En esta Jetson lo normal es /dev/ttyCH341USB0." \
            "Para ver que hay conectado: ls -l /dev/ttyCH341USB* /dev/ttyUSB* /dev/ttyACM*"
    fi
    if [[ ! -c "$PUERTO_FORZADO" ]]; then
        morir "'${PUERTO_FORZADO}' existe pero no es un dispositivo de caracteres." \
            "No parece un puerto serie valido."
    fi
    PUERTO="$PUERTO_FORZADO"
    ok "Usando el puerto forzado por vos: ${PUERTO}"
else
    for candidato in "${PUERTOS_CANDIDATOS[@]}"; do
        # Los globs sin coincidencias quedan literales; -c los descarta.
        if [[ -c "$candidato" ]]; then
            PUERTO="$candidato"
            break
        fi
    done

    if [[ -z "$PUERTO" ]]; then
        error "No encontre ningun puerto serie donde pueda estar la ESP32."
        cat >&2 <<'SINPUERTO'

        Busque, en este orden:
            /dev/ttyCH341USB0   (lo normal en esta Jetson, driver vendor WCH)
            /dev/ttyCH341USB*
            /dev/ttyUSB*        (driver ch341 del kernel)
            /dev/ttyACM*        (placas con USB nativo)

        Que revisar:
          1. Que la ESP32 este enchufada por USB a la Jetson.
          2. Que el cable USB sea de DATOS y no solo de carga. Es la causa mas
             comun: el LED de la placa enciende pero no aparece ningun /dev/tty*.
          3. Proba otro puerto USB de la Jetson.
          4. Confirma que el sistema ve el chip CH340 (USB ID 1a86:7523):
                 lsusb | grep -i 1a86
          5. Mira los mensajes del kernel al enchufarla:
                 sudo dmesg -w
             y volve a conectar el cable.
          6. Si lsusb la ve pero no aparece el nodo, revisa el modulo del driver:
                 lsmod | grep -i ch34
                 sudo modprobe ch341

        Ojo: NO uses 'pio device list' para buscar el puerto. En esta Jetson el
        driver vendor de WCH registra el puerto en el bus "usb" y pyserial lo
        devuelve vacio, asi que esa lista SIEMPRE sale vacia aunque la placa
        este perfectamente conectada.

SINPUERTO
        exit 1
    fi
    ok "Puerto detectado: ${PUERTO}"
fi

# ---------------------------------------------------------------------------
# 4. Verificar permisos sobre el puerto
# ---------------------------------------------------------------------------

titulo "Verificando permisos"

if [[ -r "$PUERTO" && -w "$PUERTO" ]]; then
    ok "Tenes permisos de lectura/escritura sobre ${PUERTO}"
else
    GRUPO_PUERTO="$(stat -c '%G' "$PUERTO" 2>/dev/null || echo 'dialout')"
    error "No tenes permisos de lectura/escritura sobre ${PUERTO}"
    printf '        Propietario/grupo actual: %s\n' \
        "$(stat -c '%U:%G (%A)' "$PUERTO" 2>/dev/null || echo 'desconocido')" >&2
    printf '        Tus grupos: %s\n\n' "$(id -nG)" >&2

    if id -nG | tr ' ' '\n' | grep -qx "$GRUPO_PUERTO"; then
        cat >&2 <<PERMISOS
        Ya pertenecas al grupo '${GRUPO_PUERTO}', asi que probablemente la sesion
        actual todavia arrastra los grupos viejos. Solucion:

            - Cerra sesion y volve a entrar, o
            - Ejecuta:  newgrp ${GRUPO_PUERTO}
            - Y verifica con:  id -nG

PERMISOS
    else
        cat >&2 <<PERMISOS
        No pertenecas al grupo '${GRUPO_PUERTO}'. Agregate y volve a iniciar sesion:

            sudo usermod -a -G ${GRUPO_PUERTO} \$USER
            # cerra sesion y volve a entrar (o ejecuta: newgrp ${GRUPO_PUERTO})
            id -nG    # deberia listar ${GRUPO_PUERTO}

        Parche temporal (se pierde al desconectar la placa):

            sudo chmod 666 ${PUERTO}

PERMISOS
    fi
    exit 1
fi

# ---------------------------------------------------------------------------
# 5. Build limpio (opcional)
# ---------------------------------------------------------------------------

if [[ "$BUILD_LIMPIO" -eq 1 ]]; then
    titulo "Limpiando build previo"
    if ! "$PIO_BIN" run --project-dir "$PROJECT_DIR" --environment "$PIO_ENV" --target clean; then
        morir "Fallo la limpieza del build." \
            "Como alternativa podes borrar el directorio a mano:" \
            "    rm -rf '${PROJECT_DIR}/.pio'"
    fi
    ok "Build previo limpiado."
fi

# ---------------------------------------------------------------------------
# 6. Compilar
# ---------------------------------------------------------------------------

titulo "Compilando"

if [[ ! -d "${PROJECT_DIR}/.pio" ]]; then
    info "Es la primera compilacion en esta maquina."
    info "PlatformIO va a descargar la toolchain de Xtensa, el framework Arduino"
    info "y las librerias (ESP32Servo). Puede tardar varios minutos y necesita internet."
fi

if ! "$PIO_BIN" run --project-dir "$PROJECT_DIR" --environment "$PIO_ENV"; then
    error "Fallo la COMPILACION (no se llego a tocar la placa)."
    cat >&2 <<'ERRCOMPILA'

        Que revisar:
          - Lee el primer error de compilacion de arriba: suele ser un error de
            sintaxis o un #include que falta en src/.
          - Si el fallo es bajando dependencias o la toolchain, revisa tu conexion
            a internet y volve a intentar.
          - Si sospechas de artefactos viejos, forza un build limpio:
                ./flashear.sh --clean
          - Para actualizar las librerias declaradas en platformio.ini:
                pio pkg update

ERRCOMPILA
    exit 1
fi

ok "Compilacion exitosa."

# ---------------------------------------------------------------------------
# 7. Flashear (SIEMPRE con el puerto explicito)
# ---------------------------------------------------------------------------

titulo "Flasheando en ${PUERTO}"

info "Se pasa el puerto de forma EXPLICITA a PlatformIO (--upload-port)."
info "La autodeteccion de esptool/pyserial no funciona con el driver WCH de esta Jetson."

if ! "$PIO_BIN" run \
        --project-dir "$PROJECT_DIR" \
        --environment "$PIO_ENV" \
        --target upload \
        --upload-port "$PUERTO"
then
    error "Fallo el FLASHEO en ${PUERTO}."
    cat >&2 <<ERRFLASH

        Causa mas frecuente: error de sincronizacion con el bootloader
        ("Failed to connect to ESP32", "Wrong boot mode detected",
         "A fatal error occurred: Failed to connect").

        >>> En varios DevKit v1 hay que entrar al bootloader A MANO: <<<
            1. Manten APRETADO el boton BOOT (a veces rotulado FLASH/IO0).
            2. Sin soltarlo, volve a lanzar:  ./flashear.sh
            3. Segui apretando BOOT hasta que aparezca "Connecting......"
               y empiece a escribir. Recien ahi soltalo.
            (Si la placa tiene boton EN/RST: con BOOT apretado, toca EN una vez
             y despues solta EN, y por ultimo BOOT.)

        Otras cosas que revisar:
          - Que el puerto no este ocupado por otro programa (un monitor serie
            abierto, screen, minicom, el monitor del IDE...). Cerralos y reintenta.
            Podes ver quien lo tiene tomado con:
                sudo fuser -v ${PUERTO}
          - Cable USB de datos, no de solo carga.
          - Alimentacion insuficiente: si la placa tiene mucho conectado (servos,
            leds), alimentala aparte antes de flashear.
          - Velocidad de subida demasiado alta: agrega a platformio.ini, dentro de
            [env:${PIO_ENV}]:
                upload_speed = 115200
          - Forzar otro puerto:
                ./flashear.sh --port /dev/ttyUSB0

ERRFLASH
    exit 1
fi

ok "Firmware flasheado correctamente en ${PUERTO}"

# ---------------------------------------------------------------------------
# 8. Monitor serie (opcional)
# ---------------------------------------------------------------------------

if [[ "$ABRIR_MONITOR" -eq 1 ]]; then
    # Velocidad del monitor segun platformio.ini, con 115200 por defecto.
    MONITOR_BAUD="$(sed -n 's/^[[:space:]]*monitor_speed[[:space:]]*=[[:space:]]*\([0-9]\+\).*/\1/p' "$INI_FILE" | head -n1)"
    MONITOR_BAUD="${MONITOR_BAUD:-115200}"

    titulo "Monitor serie (${PUERTO} @ ${MONITOR_BAUD} baudios)"
    info "Sali con Ctrl+C.  (En PlatformIO tambien sirve Ctrl+])"
    info "Si no ves nada, toca el boton EN/RST de la placa para reiniciarla"
    info "y que vuelva a imprimir el log de arranque."
    printf '\n'

    # Igual que en el flasheo: el puerto va EXPLICITO, no por autodeteccion.
    # --rts 0 --dtr 0 dejan las lineas de control en reposo: en el CH340, si RTS
    # queda activo, la ESP32 se queda RETENIDA EN RESET y no imprime nada.
    set +e
    "$PIO_BIN" device monitor \
        --project-dir "$PROJECT_DIR" \
        --environment "$PIO_ENV" \
        --port "$PUERTO" \
        --baud "$MONITOR_BAUD" \
        --rts 0 \
        --dtr 0
    ESTADO_MONITOR=$?
    set -e

    # 0 = salida normal; 130 = Ctrl+C, que tambien es una salida normal.
    if [[ $ESTADO_MONITOR -ne 0 && $ESTADO_MONITOR -ne 130 ]]; then
        aviso "El monitor serie termino con codigo ${ESTADO_MONITOR}."
        aviso "Si dice que el puerto esta ocupado, cerra cualquier otra terminal serie."
        aviso "Podes abrirlo despues a mano con:"
        aviso "    pio device monitor --port ${PUERTO} --baud ${MONITOR_BAUD}"
    fi
else
    printf '\n'
    info "Para ver la salida de la placa:"
    info "    ./flashear.sh --monitor        (compila, flashea y monitorea)"
    info "    pio device monitor --port ${PUERTO} --baud 115200   (solo monitorear)"
fi

printf '\n%s[LISTO]%s Todo terminado.\n' "$C_VERDE$C_NEGRITA" "$C_OFF"
