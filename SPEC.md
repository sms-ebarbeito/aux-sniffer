# ESP32 DP-AUX sniffer → control de backlight (especificación para firmware)

## 1. Objetivo

Adaptador pasivo USB-C/miniDP → eDP con un panel 1920x1200. El adaptador **nunca apaga el backlight**: cuando la Mac duerme la pantalla o se suspende, el panel queda negro pero iluminado.

El ESP32 escucha el canal **DisplayPort AUX** (solo escucha, nunca transmite), detecta la escritura del host al registro DPCD **0x00600 (SET_POWER)** y controla la línea **BL_EN** del panel:

| Escritura detectada | Significado | Acción |
|---|---|---|
| `0x600 = 0x01` | D0, panel encendido | Backlight **ON** |
| `0x600 = 0x02` | D3, panel apagado | Backlight **OFF** |
| `0x600 = 0x05` | D3 con AUX activo (DP 1.4; no observado, pero tratar igual) | Backlight **OFF** |

Todo esto está **verificado con capturas reales** con un analizador lógico, una Mac mini y una MacBook (mismo macOS). Ver la sección 6.

## 2. Hardware (ya armado y probado)

```
vía AUX+ (pin 15 conector eDP 40p) ──1kΩ──||10nF──► A  ┐
vía AUX- (pin 16 conector eDP 40p) ──1kΩ──||10nF──► B  ├ módulo MAX485 (Arduino) a 3,3 V
                                                       │  R7 (120Ω), R5 y R6 (bias) REMOVIDAS
                                    RE, DE ──► GND     │  RE y DE a GND = solo receptor
                                    RO ─────────────────┴──► GPIO_AUX_IN del ESP32
VCC módulo = 3V3 del ESP32 · GND común con el adaptador
```

- RO es una señal digital de 0 / 3,3 V: va **directo** al GPIO, sin divisor.
- El acople AC (10 nF) es imprescindible. Con acople DC el receptor solo producía glitches.

**Salida de backlight, cableado final (montado y probado con una ESP32-C3 SuperMini, modo `BL_MODE_DIRECT_OD`):**

```
                              pista BL_EN
 adaptador (original) ────────────╳──────────┬─────────────┬──── BL_EN del panel
                                cortada      │             │
                                           10 kΩ         GPIO_BL (GPIO5)
                                    (externa, lado panel)  open-drain, sin pulls internos
                                             │
                                     3,3 V del adaptador

ESP32 (pin 3V3) y MAX485 alimentados del mismo 3,3 V del adaptador · GND común
```

- La pista de BL_EN está **cortada**: el adaptador ya no maneja BL_EN. Del lado del panel, una **pull-up externa de 10 kΩ** al 3,3 V del adaptador la mantiene en alto, y el GPIO va directo a esa línea.
- **GPIO_BL en LOW** (salida open-drain activa): BL_EN queda a GND y el **backlight se apaga**. El pin absorbe unos 0,33 mA.
- **GPIO_BL suelto** (open-drain en alto, alta impedancia, o pin sin configurar): la pull-up externa lleva BL_EN a 3,3 V y el **backlight queda encendido**. Es el estado seguro.
- **Por qué la pull-up es externa y no la interna del ESP32:** la interna solo existe cuando el ESP32 corre el firmware y la configura. Durante el reset, el bootloader o la grabación, después de un cuelgue, o con el chip mantenido en reset, el pin queda en su estado por defecto y BL_EN quedaría flotando, con el backlight en un estado indefinido. La de 10 kΩ, en cambio, no depende del ESP32: mientras haya 3,3 V en el adaptador, BL_EN está en alto salvo que el GPIO lo baje activamente. Además, la interna ronda los 45 kΩ y es más sensible al ruido. Por eso el firmware configura el pin **sin pull-up ni pull-down internos**: la línea queda definida solo por la resistencia externa.
- **El ESP32 se alimenta del mismo 3,3 V que la pull-up.** Si el ESP32 quedara sin tensión mientras el adaptador sigue alimentado (por ejemplo, con el cable de 3V3 suelto), el diodo de protección del GPIO llevaría BL_EN a alrededor de 0,6 V a través de los 10 kΩ y el backlight se apagaría. Con un solo riel, los dos se encienden y se apagan juntos.

**Alternativa con MOSFET (soportada por el firmware como `BL_MODE_MOSFET`, no montada):**

```
GPIO_BL ──100Ω──┬── gate 2N7002
               100kΩ
                │
               GND        drain ── BL_EN (con su pull-up)
                          source ── GND
```

- **GPIO_BL en HIGH**: el MOSFET conduce, BL_EN queda a GND y el **backlight se apaga**.
- **GPIO_BL en LOW o sin configurar**: gracias al pulldown de 100 kΩ, el **backlight queda encendido**. Sirve si el ESP32 tiene una alimentación separada (con GND común).

**Pines sugeridos.** Hay que evitar los pines de strapping. Para AUX_IN, el pin tiene que poder rutearse al RMT (cualquier GPIO sirve).

| Placa | GPIO_AUX_IN | GPIO_BL | Evitar |
|---|---|---|---|
| ESP32 (WROOM) | 4 | 25 | 0, 2, 5, 12, 15, 6–11 |
| ESP32-S3 | 4 | 5 | 0, 3, 45, 46, 19/20 (USB), 26–32 (flash/PSRAM) |
| ESP32-C3 | 4 | 5 | 2, 8, 9, 18/19 (USB) |

## 3. Características de la señal en RO (medidas)

- **Codificación:** Manchester-II a 1 Mbps. Bit = 1 µs y medio bit = 0,5 µs.
- **Asimetría:** las mitades no son simétricas por el umbral del receptor. Se observaron pulsos de **375–625 ns** para medio bit y **875–1125 ns** para bit completo. El decodificador tiene que clasificar por rangos, no por valor exacto.
- **Nivel en reposo indefinido:** con el acople AC, RO puede quedar en alto o en bajo entre tramas. **No hay que asumir un nivel de reposo.**
- **Ruido en reposo:** no se observó. Igual conviene descartar ráfagas de menos de 20 flancos.
- **Mensajes:** cada uno tiene esta estructura:
  - **Preámbulo:** ~16 ceros Manchester, es decir, pulsos alternados de 0,5 µs.
  - **SYNC:** 2 µs en un nivel y 2 µs en el otro (4 medios bits + 4 medios bits).
  - **Datos:** en Manchester.
  - **STOP:** mismo patrón que el SYNC.
- **Pedido y respuesta en una sola ráfaga:** la respuesta del panel llega **unos pocos µs** después del STOP del pedido. Con un umbral de inactividad de ~6–10 µs, **pedido y respuesta quedan en la misma ráfaga**, así que el decodificador debe extraer **varios mensajes por ráfaga**.
- **Polaridad con el cableado actual:**
  - SYNC = 4 medios bits **en alto** y después 4 **en bajo**.
  - Bit de datos = **nivel del primer medio bit** (alto = 1).
  - Si se invierten A y B, toda la señal sale invertida. El decodificador debe probar también el patrón de SYNC invertido y, si lo encuentra, invertir los niveles.

## 4. Algoritmo de decodificación

La referencia funcionando está en `auxdec.py`, en esta misma carpeta. Corre sobre capturas `.sr` de sigrok y es la fuente de verdad. Los pasos:

1. **Capturar** una ráfaga como una lista de corridas `(nivel, duración_ns)`.
2. **Cuantizar** cada corrida a medios bits: `u = round(dur_ns / 500)`, acotado a 1..8. En la práctica:
   - `<750 ns` → 1
   - `750–1500 ns` → 2
   - `1500–2750 ns` → 4–5
   - más largo → 6–8 (STOP + SYNC pegados).
3. **Expandir** a un arreglo de medios bits (`nivel` repetido `u` veces).
4. **Buscar el SYNC:** el patrón `1111 0000` precedido por al menos 8 medios bits de preámbulo (o `0000 1111` si la polaridad está invertida; en ese caso, invertir todo).
5. **Leer los datos:** después del SYNC, tomar pares de medios bits `(a, b)`.
   - Si `a != b`, el bit vale `a`.
   - Si `a == b`, se terminaron los datos (es el STOP).
6. **Armar los bytes:** agrupar de a 8 bits, MSB primero, y descartar el resto si los bits no son múltiplo de 8.
7. **Seguir buscando** otro SYNC en lo que queda de la ráfaga (la respuesta).

**Formato de un pedido (fuente → panel):**

```
byte0 = CMD[3:0] << 4 | ADDR[19:16]
byte1 = ADDR[15:8]
byte2 = ADDR[7:0]
byte3 = LEN - 1
byte4.. = datos (solo en escrituras)
CMD: 1000 = native write, 1001 = native read, 0xxx = I2C-over-AUX
```

**La trama a detectar es exactamente esta, de 5 bytes:**

```
80 06 00 00 XX     → native write, addr 0x00600, len 1, data XX
```

- `XX = 01` → backlight ON.
- `XX = 02` o `05` → backlight OFF.

Cualquier otra cosa se ignora. Una respuesta nunca tiene 5 bytes que empiecen con `80 06 00 00`: la respuesta a una escritura es un solo byte `00` (ACK). Por eso basta con exigir los 5 bytes exactos.

## 5. Requisitos del firmware

- **Framework:** Arduino-ESP32 **core 3.x** (ESP-IDF 5.x), usando directamente `driver/rmt_rx.h`, que es el driver RMT nuevo.
- **Captura con RMT RX:**
  - **Resolución:** 10–20 MHz (50–100 ns por tick).
  - **`signal_range_min_ns`:** ~100–150 ns (filtro de glitches; el máximo del hardware ronda los 3 µs).
  - **`signal_range_max_ns`:** ~10 000 ns (fin de ráfaga tras 10 µs sin flancos).
  - **Buffer:** al menos **256 símbolos**. Una ráfaga de escritura más respuesta tiene unas 195 corridas, unos 100 símbolos RMT. Las lecturas de EDID son más largas: si desbordan, se descartan sin problema.
  - **Memoria RMT:** en el ESP32 clásico cada canal tiene 64 símbolos por bloque, así que hay que asignar varios bloques (`mem_block_symbols`). En el S3 se puede usar DMA.
  - **Rearmado:** volver a llamar a `rmt_receive()` **enseguida**. Usar doble buffer y pasar la ráfaga por una cola (`xQueueSendFromISR` desde el callback `on_recv_done`) a una tarea que decodifica. Entre ráfagas hay ≥400 µs, que sobran para rearmar.
- **Lógica:**
  - **Al arrancar:** backlight encendido lo antes posible. En modo directo (el montado), `GPIO_BL` en open-drain, suelto y sin pulls internos. En modo MOSFET, `GPIO_BL` en LOW.
  - **Al detectar `80 06 00 00 02` o `05`:** backlight OFF. En modo directo, `GPIO_BL` en LOW; en modo MOSFET, en HIGH.
  - **Al detectar `80 06 00 00 01`:** backlight ON. En modo directo, `GPIO_BL` suelto; en modo MOSFET, en LOW.
  - No hace falta antirrebote: cada evento es una escritura única.
- **Depuración por Serial (115200):** en modo debug, imprimir cada pedido decodificado (dirección, comando, datos), nunca desde la ISR o el callback. Indicar el cambio de estado con el LED de la placa.
- **Watchdog** habilitado. Si algo falla, el reset suelta el pin y el backlight queda encendido: por la pull-up externa de 10 kΩ en modo directo, o por el pulldown del gate en modo MOSFET.

## 6. Secuencias reales observadas (macOS, Mac mini y MacBook)

**Reposo de pantalla** (`pmset displaysleepnow`). macOS espera **~10 s con la pantalla negra** antes de mandar el D3:

```
 3,00 s   (pantalla negra)
13,10 s   Native WRITE 0x00600 data=02        ← apagar backlight
35,04 s   Native READ  0x00600 len=1          ← al despertar
35,04 s   Native WRITE 0x00600 data=01        ← prender backlight
35,05 s   link training: WRITE 0x107=00, 0x100=0a, 0x101=82, 0x102=21/22/00, READ 0x202...
35,12 s   READ 0x6921D, 0x68028 (registros propios de Apple), WRITE 0x20F=07, 0x2011=07
```

**Suspensión del sistema:** `WRITE 0x00600 data=02` en el momento de dormirse, y después silencio total.

**Conexión del cable / despertar del sistema:** lectura de capacidades DPCD (`0x000` len 16, `0x400`, `0x500`...), lectura del EDID por I2C-over-AUX (`@0x50`, 8 × 16 bytes), sondeo de `@0x52` (NACK), `WRITE 0x00600 data=01` y link training.

**Desconexión del cable:** no genera nada por AUX. En la práctica, si el ESP32 se alimenta del adaptador, se apaga junto con él.

## 7. Vectores de prueba

El archivo `testvectors_0x600.json` tiene las **corridas reales** (`[nivel, duración_ns]`, capturadas a 8 MHz, así que la resolución es de 125 ns) de tres ráfagas:

| t (s) | Bytes del pedido | Significado |
|---|---|---|
| 13,100 | `80 06 00 00 02` | WRITE 0x600 = 02 (pedido + respuesta ACK en la misma ráfaga) |
| 35,044 | `90 06 00 00` | READ 0x600 (con su respuesta) |
| 35,044 | `80 06 00 00 01` | WRITE 0x600 = 01 (pedido + respuesta ACK) |

Sirven para probar el decodificador en C sin hardware: convertir las corridas a un arreglo, pasarlas por la función de decodificación y verificar que salgan exactamente esos bytes y la acción correcta.

## 8. Ideas para después (fuera de este alcance)

- **Brillo PWM:** cortar la pista del PWM de 2 kHz (sale del pin 5 del MCU de 8 pines que está sobre el jog) y que el ESP32 genere el PWM con LEDC, leyendo la rueda o el PWM original como "brillo pedido".
- **Control desde la compu:** por WiFi (HTTP o MQTT), o leyendo comandos DDC/CI de brillo (I2C-over-AUX a `@0x37`, VCP 0x10). Lo segundo requiere que alguien conteste el ACK, porque si no macOS no los manda.
