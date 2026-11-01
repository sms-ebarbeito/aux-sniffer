# aux-sniffer

Firmware para ESP32 que escucha el canal DisplayPort AUX de forma pasiva y
apaga o prende el backlight del panel según las escrituras del host al
registro DPCD `0x600` (SET_POWER). La especificación completa está en
[SPEC.md](SPEC.md). La placa principal es la **ESP32-C3 SuperMini**; ESP32
(WROOM) y ESP32-S3 compilan, pero son secundarias.

```
firmware/aux_sniffer/
  aux_sniffer.ino   RMT RX, tareas, backlight, Serial
  config.h          ÚNICO lugar a tocar: placa, pines, modo de backlight, RMT
  auxdec.h/.cpp     decodificador AUX sin dependencias de Arduino
test/               test de host (clang) con las ráfagas reales
build.sh            compila para c3 / s3 / wroom
```

## Test del decodificador (en la Mac, sin hardware)

```sh
make -C test
```

Corre las tres ráfagas reales de `testvectors_0x600.json` y verifica los bytes
del pedido, la respuesta (ACK) y la acción. También prueba la polaridad
invertida, el camino completo de símbolos RMT a 10 y 20 MHz, jitter de ±100 ns,
dos transacciones en la misma ráfaga y ruido aleatorio. Además, **corta cada
ráfaga en todos los puntos posibles**, por corrida y por símbolo RMT, y
verifica que nunca salga una acción equivocada y que, una vez completo el
pedido, salga siempre la correcta:

```
t=13.100240s 80 06 00 00 02    acción correcta desde 139/193 corridas,  70/97 símbolos RMT
t=35.044497s 80 06 00 00 01    acción correcta desde 140/195 corridas,  70/98 símbolos RMT
```

## Compilar

```sh
./build.sh          # ESP32-C3 SuperMini (por defecto)
./build.sh all      # c3, s3 y wroom
```

El script usa estos FQBN:

| Placa | FQBN |
|---|---|
| C3 SuperMini | `esp32:esp32:esp32c3:CDCOnBoot=cdc` |
| S3 | `esp32:esp32:esp32s3:CDCOnBoot=cdc` |
| WROOM | `esp32:esp32:esp32` |

En la SuperMini, **`CDCOnBoot=cdc` es obligatorio**: la placa no tiene chip
USB-serie y el Serial sale por el USB nativo (GPIO18/19). El binario queda en
`build/<placa>/`.

Para cambiar la configuración sin editar `config.h`, por ejemplo para usar el
modo MOSFET:

```sh
EXTRA="-DBL_MODE=BL_MODE_MOSFET" ./build.sh
```

> **Apple Silicon sin Rosetta:** el `ctags` que instala arduino-cli es x86_64
> y no corre. `build.sh` lo detecta y usa `tools/ctags-noop`, que no hace
> nada. El sketch no necesita los prototipos que genera ctags. Si se instala
> Rosetta (`softwareupdate --install-rosetta`), el script vuelve solo a usar el
> ctags normal. Para compilar a mano sin el script, hay que agregar
> `--build-property "tools.ctags.path=$PWD/tools/ctags-noop"`.

## Flashear

```sh
arduino-cli board list          # buscar el puerto, p. ej. /dev/cu.usbmodem1101
./build.sh c3
arduino-cli upload -p /dev/cu.usbmodem1101 \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --input-dir build/c3
arduino-cli monitor -p /dev/cu.usbmodem1101 -c baudrate=115200
```

Si la SuperMini no entra en modo de grabación, mantener apretado **BOOT**
(GPIO9), tocar **RESET** y soltar BOOT. Después de grabar, un RESET la arranca
normalmente.

## Cableado (C3 SuperMini)

### Receptor AUX (igual que en el SPEC)

```
AUX+ (pin 15 eDP) ──1kΩ──||10nF──► A ┐
AUX- (pin 16 eDP) ──1kΩ──||10nF──► B ├ MAX485 a 3,3 V (sin R7, R5 ni R6)
                RE, DE ──► GND       │
                RO ──────────────────┴──► GPIO4 (PIN_AUX_IN)
VCC MAX485 = 3V3 · GND común
```

Si A y B quedan invertidos, no pasa nada: el decodificador detecta la
polaridad en cada ráfaga.

### Backlight: cableado final (modo directo, `BL_MODE_DIRECT_OD`, el default)

Montado y probado: el backlight se apaga con el reposo de pantalla y vuelve
al despertar.

```
                              pista BL_EN
 adaptador (original) ────────────╳──────────┬─────────────┬──── BL_EN del panel
                                cortada      │             │
                                           10 kΩ         GPIO5 (PIN_BL)
                                    (externa, lado panel)  open-drain, sin pulls
                                             │
                                     3,3 V del adaptador

3,3 V del adaptador ──► 3V3 de la SuperMini y VCC del MAX485
GND del adaptador ────► GND
```

- **La pista de BL_EN está cortada.** El adaptador ya no maneja BL_EN: lo
  definen solo la pull-up externa de 10 kΩ, soldada del lado del panel, y el
  GPIO5.
- **Backlight OFF:** el GPIO pone LOW y absorbe unos 0,33 mA por los 10 kΩ.
- **Backlight ON:** el GPIO queda suelto, en open-drain y alta impedancia, y
  la pull-up lleva BL_EN a 3,3 V.
- **Por qué la pull-up es externa:** una pull-up interna del ESP32 solo existe
  mientras el firmware corre y la configura. Durante el reset, el bootloader
  o la grabación, después de un cuelgue, o con el chip en reset, el pin queda
  en su estado por defecto y BL_EN quedaría flotando. La de 10 kΩ no depende
  del ESP32, así que **en cualquiera de esos casos el backlight queda
  encendido**. Por eso el firmware configura el pin **sin pulls internos**,
  en `initVariant()`, antes de `setup()`. Ante un watchdog, un panic o un
  `esp_restart()`, el pin vuelve a quedar suelto.
- **El ESP32 tiene que alimentarse del mismo 3,3 V que la pull-up.** Si el
  ESP32 quedara sin tensión con el adaptador encendido (por ejemplo, con el
  cable de 3V3 suelto), el diodo de protección del GPIO llevaría BL_EN a unos
  0,6 V a través de los 10 kΩ y el backlight se apagaría. La SuperMini se
  alimenta por el pin **3V3**, sin pasar por su regulador.

### Backlight: alternativa con MOSFET (`BL_MODE_MOSFET`, no montada)

```
GPIO5 (PIN_BL) ──100Ω──┬── gate 2N7002
                      100kΩ             drain ── BL_EN (con su pull-up)
                       │                source ── GND
                      GND
```

- **HIGH:** el MOSFET conduce y el **backlight se apaga**.
- **LOW, o el pin sin configurar:** el pulldown de 100 kΩ deja el **backlight
  encendido**.

Sirve si el ESP32 tiene su propia alimentación, con GND común. Se compila
con `EXTRA="-DBL_MODE=BL_MODE_MOSFET" ./build.sh`.

### LED

El LED azul de la SuperMini está en GPIO8, activo en bajo. **Encendido
significa que el ESP32 está apagando el backlight.** Al arrancar parpadea 3
veces. GPIO8 es de strapping: solo se usa como salida después del arranque.

## Qué se ve por Serial (115200, modo debug)

La depuración viene activada (`AUX_DEBUG 1` en `config.h`). Comandos por la
consola: **`d`** activa o desactiva el debug, **`s`** muestra estadísticas y
**`h`** repite el encabezado.

Ejemplo armado con las secuencias del SPEC §6 (todavía no es una captura real
del ESP32):

```
aux-sniffer | ESP32-C3 SuperMini | AUX_IN=GPIO4 BL=GPIO5 (directo open-drain, LOW=OFF) LED=GPIO8
RMT 10000000 Hz, mem 96 símbolos, buffer 4 x 512, filtro 150 ns, fin de ráfaga 10000 ns
debug=on  (teclas: d=debug on/off, s=stats, h=ayuda)
[     13.100] REQ  Native WRITE 0x00600 len=1 data=02
[     13.100]      REPLY ACK
[     13.100] >>> BACKLIGHT OFF
[     35.043] REQ  Native READ  0x00600 len=1
[     35.043]      REPLY ACK data=02
[     35.044] REQ  Native WRITE 0x00600 len=1 data=01
[     35.044]      REPLY ACK
[     35.044] >>> BACKLIGHT ON
[     35.050] REQ  Native WRITE 0x00100 len=1 data=0a
...
stats: ráfagas=412 ruido=0 sin_sync=0 truncadas=0 mensajes=824 descartadas=0 log_perdido=0 rearmados=0 | backlight=ON rmt=ok
```

- **Tiempo:** segundos desde el arranque.
- **`REQ`:** el pedido de la fuente. La línea siguiente es la respuesta del
  panel, que viene en la misma ráfaga.
- **`>>> BACKLIGHT`:** aparece siempre, aunque el debug esté apagado.
- **`?? ráfaga sin SYNC`:** una ráfaga de 20 corridas o más que no se pudo
  decodificar.
- **`stats`:** se imprime cada 10 s si hubo tráfico. Los contadores son:
  - `truncadas`: ráfagas que no entraron en el buffer.
  - `descartadas`: ráfagas perdidas porque la cola estaba llena.
  - `log_perdido`: líneas de debug que no se imprimieron. Nunca afecta la
    acción.
  - `rearmados`: veces que hubo que reiniciar el RMT.

El Serial se imprime desde `loop()`, que tiene prioridad baja, a través de una
cola. Si el puerto es lento o nadie lo está leyendo, se pierden líneas de log,
pero nunca se demora el control del backlight.

## Cómo entra una ráfaga en el RMT de la C3

El RMT de la C3 tiene 2 canales RX con 48 símbolos de memoria cada uno y
**no tiene DMA**. Una escritura con su ACK ocupa unos 98 símbolos, y el pedido
solo, 70.

1. **Dos bloques encadenados.** El canal RX usa `mem_block_symbols = 96`, es
   decir, su bloque más el del otro canal RX, que queda sin usar.
2. **Ping-pong del driver.** El core 3.3.10 trae ESP-IDF v5.5.4, que en
   targets con `SOC_RMT_SUPPORT_RX_PINGPONG` (C3 y S3) usa la memoria RMT como
   ping-pong. En cada mitad (48 símbolos, unos 50 a 100 µs de señal), una
   interrupción copia los datos al buffer de usuario, que es de 512 símbolos.
   Así entra la ráfaga completa, incluso una lectura de 16 bytes, que ronda
   los 210 símbolos. Lo verifiqué en `soc_caps.h` y en los símbolos de
   `libesp_driver_rmt.a` (`ping_pong_symbols`, `en_partial_rx`). No usé
   `flags.en_partial_rx` (que también está disponible): sirve cuando el
   buffer de usuario es más chico que la ráfaga, y acá no lo es.
3. **Tolerancia a cortes.** Si por alguna razón la ráfaga se corta, el
   decodificador procesa lo que llegó. Por ejemplo, si el ping-pong no
   alcanzara y solo quedaran los 96 símbolos de la memoria, alcanza igual: el
   pedido se completa en el símbolo 70. El test de host lo verifica cortando
   en cada símbolo.

Para recibir ráfagas sin perder ninguna, la ISR (`on_recv_done`) rearma el RMT
enseguida con el siguiente buffer de un anillo de 4 y pasa la ráfaga a una
tarea de alta prioridad por una cola. Esa tarea decodifica y mueve el pin.

## Estado en hardware

**Probado** con la ESP32-C3 SuperMini y el cableado final (modo directo con
pull-up externa de 10 kΩ): el backlight se apaga con el reposo de pantalla y
vuelve al despertar.

**Sin verificar todavía:**

- **Suspensión del sistema.** El SPEC §6 dice que manda el mismo
  `WRITE 0x600 = 02`.
- **Contador `truncadas`.** Mostraría si el ping-pong del RMT entrega
  ráfagas completas también en las lecturas largas. La escritura a `0x600`
  ya se decodifica bien.
- **WROOM y S3.** Compilan, pero no se probaron. En la WROOM, que no tiene
  ping-pong, las ráfagas de más de 256 símbolos (lecturas de EDID) deberían
  truncarse sin afectar a la `0x600`.
