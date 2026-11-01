// Configuración del sniffer: placa, pines, modo de backlight y RMT.
// Es el ÚNICO lugar a tocar para adaptar el firmware. La placa se elige sola
// según el FQBN con el que se compila; cualquier valor se puede pisar con -D
// (por ejemplo EXTRA="-DPIN_AUX_IN=6" ./build.sh, que usa
// --build-property "compiler.cpp.extra_flags=...").
#pragma once

#include "sdkconfig.h"

// ---------------------------------------------------------------- backlight
// BL_MODE_DIRECT_OD (default, el montado): pista de BL_EN cortada, el GPIO va
//   directo a BL_EN del lado panel, con pull-up externa de 10 kΩ al 3,3 V del
//   adaptador. Open-drain sin pulls internos: LOW = backlight OFF; soltar el
//   pin (alta impedancia) = ON, lo sube la pull-up externa. Es externa para
//   que BL_EN quede en alto aunque el ESP32 esté en reset o sin firmware.
//   El ESP32 tiene que alimentarse del mismo 3,3 V del adaptador.
// BL_MODE_MOSFET (SPEC §2): GPIO -> 100 Ω -> gate 2N7002 (100 kΩ a GND).
//   HIGH = backlight OFF, LOW = ON.
// En ambos modos el estado de arranque, reset o error es backlight ON.
#define BL_MODE_MOSFET 1
#define BL_MODE_DIRECT_OD 2
#ifndef BL_MODE
#define BL_MODE BL_MODE_DIRECT_OD
#endif

// ---------------------------------------------------------------- placa
#if CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 SuperMini (placa principal). Evitar 2, 8, 9 (strapping) y 18/19
// (USB nativo: por ahí sale el Serial con CDC On Boot).
#define BOARD_NAME "ESP32-C3 SuperMini"
#ifndef PIN_AUX_IN
#define PIN_AUX_IN 4
#endif
#ifndef PIN_BL
#define PIN_BL 5
#endif
#ifndef PIN_LED
#define PIN_LED 8  // LED azul; strapping, solo se usa como salida tras el arranque
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif
// Memoria RMT del canal RX: los 2 bloques RX de 48 símbolos (el del canal y
// el del otro canal RX). El driver los usa como ping-pong y va copiando al
// buffer de usuario, así que la ráfaga puede ser más larga que esto.
#ifndef RMT_MEM_SYMBOLS
#define RMT_MEM_SYMBOLS 96
#endif

#elif CONFIG_IDF_TARGET_ESP32S3
#define BOARD_NAME "ESP32-S3"
#ifndef PIN_AUX_IN
#define PIN_AUX_IN 4
#endif
#ifndef PIN_BL
#define PIN_BL 5
#endif
#ifndef PIN_LED
#define PIN_LED -1  // el LED de las DevKit S3 es RGB (WS2812): no se usa
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif
#ifndef RMT_MEM_SYMBOLS
#define RMT_MEM_SYMBOLS 96  // 2 bloques de 48, ping-pong como en la C3
#endif

#elif CONFIG_IDF_TARGET_ESP32
#define BOARD_NAME "ESP32 (WROOM)"
#ifndef PIN_AUX_IN
#define PIN_AUX_IN 4
#endif
#ifndef PIN_BL
#define PIN_BL 25
#endif
#ifndef PIN_LED
#define PIN_LED 2  // LED de muchas DevKit; strapping, solo salida tras el arranque
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif
// El ESP32 clásico no tiene ping-pong en RX: la ráfaga entera tiene que
// entrar en la memoria del canal. 4 bloques de 64 = 256 símbolos.
#ifndef RMT_MEM_SYMBOLS
#define RMT_MEM_SYMBOLS 256
#endif

#else
#error "Placa no soportada: agregar sus pines en config.h"
#endif

// ---------------------------------------------------------------- RMT
#define RMT_RESOLUTION_HZ 10000000  // 100 ns por tick
#define RMT_MIN_NS 150              // pulsos más cortos = glitch
#define RMT_MAX_NS 10000            // 10 µs sin flancos = fin de ráfaga
// Buffer de usuario por ráfaga. Escritura + ACK ≈ 100 símbolos; la ráfaga más
// larga posible de AUX (16 bytes de datos) ronda los 210.
#define RX_BUF_SYMBOLS 512
#define NUM_RX_BUFS 4  // anillo de buffers entre el callback y la tarea

// ---------------------------------------------------------------- varios
#ifndef AUX_DEBUG
#define AUX_DEBUG 1  // imprimir cada mensaje por Serial (se alterna con 'd')
#endif
#define SERIAL_BAUD 115200
#define WDT_TIMEOUT_MS 5000

// ---------------------------------------------------------------- chequeos
#if CONFIG_IDF_TARGET_ESP32C3
#define C3_FORBIDDEN(p) ((p) == 2 || (p) == 8 || (p) == 9 || (p) == 18 || (p) == 19)
static_assert(!C3_FORBIDDEN(PIN_AUX_IN), "PIN_AUX_IN: GPIO de strapping o USB en la C3");
static_assert(!C3_FORBIDDEN(PIN_BL), "PIN_BL: GPIO de strapping o USB en la C3");
#endif
static_assert(PIN_AUX_IN != PIN_BL, "PIN_AUX_IN y PIN_BL tienen que ser distintos");
static_assert(BL_MODE == BL_MODE_MOSFET || BL_MODE == BL_MODE_DIRECT_OD, "BL_MODE inválido");
