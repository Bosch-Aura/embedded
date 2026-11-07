# CLAUDE.md

## Si estás corriendo en la Jetson, NO correr el simulador

La Jetson es una placa chica: compilar y correr `test/sim/` (`./test/sim/run_all.sh` o cualquier
script de esa carpeta) la satura. Si el entorno es la Jetson (Linux `*-tegra`, usuario `jetson`,
`/home/jetson/...`), no lo corras. Dejalo para una PC de desarrollo y avisá que quedó sin correr.

En la Jetson sí se puede compilar y flashear el firmware (`pio run`, `./flashear.sh`).
