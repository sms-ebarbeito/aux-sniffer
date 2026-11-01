#!/bin/sh
# Compila el firmware. Uso: ./build.sh [c3|s3|wroom|all]   (default: c3)
# Variables opcionales: ARDUINO_CLI, BUILD_DIR, EXTRA (p. ej. EXTRA="-DBL_MODE=BL_MODE_MOSFET")
set -e
cd "$(dirname "$0")"
CLI=${ARDUINO_CLI:-/opt/homebrew/bin/arduino-cli}
BUILD_DIR=${BUILD_DIR:-build}

fqbn() {
  case $1 in
    c3) echo esp32:esp32:esp32c3:CDCOnBoot=cdc ;;  # SuperMini: Serial por USB nativo
    s3) echo esp32:esp32:esp32s3:CDCOnBoot=cdc ;;
    wroom) echo esp32:esp32:esp32 ;;
    *) echo "placa desconocida: $1" >&2; exit 2 ;;
  esac
}

# El ctags que instala arduino-cli es x86_64: en Apple Silicon sin Rosetta no
# corre. El sketch no necesita sus prototipos, así que se usa uno vacío.
CTAGS=$(ls "$HOME"/Library/Arduino15/packages/builtin/tools/ctags/*/ctags 2>/dev/null | head -1)
set --  "${@:-c3}"
PROPS=""
if [ -n "$CTAGS" ] && ! "$CTAGS" --version >/dev/null 2>&1; then
  PROPS="tools.ctags.path=$PWD/tools/ctags-noop"
fi

[ "$1" = all ] && set -- c3 s3 wroom
for board in "$@"; do
  echo "=== $board ($(fqbn "$board"))"
  "$CLI" compile --fqbn "$(fqbn "$board")" --warnings all \
    ${PROPS:+--build-property "$PROPS"} \
    ${EXTRA:+--build-property "compiler.cpp.extra_flags=$EXTRA"} \
    --build-path "$BUILD_DIR/$board" firmware/aux_sniffer
done
