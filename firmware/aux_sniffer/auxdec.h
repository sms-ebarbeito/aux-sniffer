// Decodificador DisplayPort AUX (Manchester-II, 1 Mbps) a partir de corridas
// (nivel, duración). Sin dependencias de Arduino ni de ESP-IDF: compila igual
// en el ESP32 y en el host (test/). Port de auxdec.py, extendido para extraer
// varios mensajes por ráfaga (pedido + respuesta).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace auxdec {

// AUX: 4 bytes de cabecera + hasta 16 de datos.
constexpr size_t kMaxMsgBytes = 20;
// Medios bits que se expanden por ráfaga. Si se llena, el resto se descarta
// (equivale a una ráfaga truncada).
constexpr size_t kMaxHalfbits = 4096;
// Ráfagas con menos corridas que esto se descartan como ruido (SPEC §3).
constexpr size_t kMinRuns = 20;
// Mensajes por ráfaga como máximo (normalmente 2: pedido y respuesta).
constexpr size_t kMaxMsgs = 8;
// Medios bits que se asignan a la última corrida cuando su duración es
// desconocida (dur_ns == 0: el nivel quedó quieto hasta el fin de ráfaga).
constexpr uint8_t kFinalRunUnits = 4;

struct Run {
  uint8_t level;    // 0 o 1
  uint32_t dur_ns;  // 0 = corrida final de duración desconocida
};

struct Message {
  uint8_t bytes[kMaxMsgBytes];
  uint8_t nbytes;
  uint16_t nbits;  // bits leídos; si no es múltiplo de 8 sobran nbits % 8
  bool stopped;    // true si terminó en STOP; false si la ráfaga se cortó antes
  bool inverted;   // polaridad invertida (A/B cruzados)
};

enum class Action : uint8_t { None, BacklightOn, BacklightOff };

// Duración -> medios bits, round-half-even como round() de Python, acotado a
// 8. Devuelve 0 para pulsos < 250 ns (glitch: la corrida se descarta).
uint8_t quantize(uint32_t dur_ns);

// Convierte símbolos RMT (rmt_symbol_word_t::val) a corridas. Formato de cada
// palabra: duration0 bits 0-14, level0 bit 15, duration1 bits 16-30, level1
// bit 31. Una duración 0 es la marca de fin de ráfaga del RMT: esa corrida se
// emite con dur_ns = 0 y se termina. Si no aparece (buffer truncado), todas
// las corridas tienen duración real. Devuelve la cantidad de corridas.
size_t runsFromRmt(const uint32_t* words, size_t nwords, uint32_t tick_ns,
                   Run* out, size_t max_runs);

class Decoder {
 public:
  // Decodifica todos los mensajes de una ráfaga. Prueba las dos polaridades
  // y se queda con la que da más mensajes de bytes completos (empate: la
  // normal). Devuelve la cantidad de mensajes escritos en out (como máximo
  // kMaxMsgs).
  size_t decode(const Run* runs, size_t nruns, Message* out, size_t max_msgs);

 private:
  size_t expand(const Run* runs, size_t nruns);
  size_t extract(bool inverted, Message* out, size_t max_msgs) const;
  bool syncAt(size_t i, uint8_t flip) const;

  uint8_t hb_[kMaxHalfbits];
  Message alt_[kMaxMsgs];
  size_t nhb_ = 0;
};

// Acción para un mensaje: solo "80 06 00 00 XX" de exactamente 40 bits.
// XX=01 -> ON, XX=02/05 -> OFF, cualquier otra cosa -> None.
Action classify(const Message& m);

// Texto legible para depuración (al estilo de auxdec.py). Devuelve buf.
const char* describeRequest(const Message& m, char* buf, size_t len);
const char* describeReply(const Message& m, char* buf, size_t len);

}  // namespace auxdec
