#!/bin/bash
# Compila el firmware para la PC y corre todas las pruebas del simulador.
# Necesita AURA-2026 al lado de este repo (o AURA_BRAIN=/ruta/a/AURA-2026/brain).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
cd "$HERE"
./build.sh "" build/fw_sim                                                   # driver de eco por interrupcion
SIM_FLAGS="-DULTRASONIC_USE_PULSEIN" ./build.sh "" build/fw_pulsein          # driver alternativo
SIM_FLAGS="-DSIM_TICK_OFFSET=4294964796u" ./build.sh "" build/fw_wrap        # reloj 2,5 s antes del wrap
status=0
echo "== Regresiones del firmware ==";            $PY fw_regressions.py build/fw_sim build/fw_wrap || status=1
echo "== Escenarios de la Jetson ==";             $PY jetson_real.py build/fw_sim || status=1
echo "== Regresiones de la Jetson ==";            $PY jetson_regressions.py build/fw_sim || status=1
echo "== Ultrasonido (interrupcion) ==";          $PY us_tests.py build/fw_sim || status=1
echo "== Ultrasonido (pulseIn) ==";               $PY us_tests.py build/fw_pulsein || status=1
exit $status
