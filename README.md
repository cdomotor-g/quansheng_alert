# quansheng_alert — ALERT telemetry receiver firmware for the Quansheng UV-K5

Custom firmware for the Quansheng UV-K5 / UV-K5(8) / UV-K6 family (DP32G030 MCU +
BK4819 radio) that turns the handheld into a portable **ALERT flood-warning
telemetry receiver**:

* captures the 300-baud ALERT bursts the field stations transmit on 151.500 MHz,
* decodes the 13-bit station address and 11-bit value (ALERT Binary Format and
  Enhanced IFLOWS Format, exactly as MegaNet's `packets.js` does),
* looks the address up in a station table generated from the
  [MegaNet](https://github.com/cdomotor-g/MegaNet) repository, and
* shows the station name, sensor kind, address and value on the LCD and reads the
  address and value out with the radio's voice prompts.

It also replaces the stock 0–9 squelch with a **fine 0.0–9.0 squelch** with
user-settable open/close delays, so weak data bursts open the squelch quickly and
the no-signal noise floor can be muted without muting the signals.

The base is [egzumer/uv-k5-firmware-custom](https://github.com/egzumer/uv-k5-firmware-custom)
(Apache-2.0, see `LICENSE.egzumer`; its original README is in
`docs/README-egzumer-upstream.md`). Everything egzumer offers is still here except
the features switched off in the `Makefile` to make room (FM broadcast radio,
spectrum analyser, DTMF calling, VOX, AM fix, small-bold font, scan ranges). Any of
them can be switched back on with `make ENABLE_xxx=1` if you shrink the station
table to compensate.

> **Status: built and unit-tested, not yet tried on air.** Nothing in this repo has
> been run on a real radio next to a real ALERT transmitter yet. The decoder is
> verified against MegaNet's test vectors on the host, the firmware builds and fits,
> and every radio-facing parameter is adjustable from the radio so the first field
> test can be done without reflashing. See [First field test](#first-field-test).

---

## What you get on the radio

### The ALERT RX screen

Start it with **F then 0**, from the menu item **AlrtRx**, or by assigning
`ALERT RX` to a side key (menu `F1Shrt` / `F1Long` / `F2Shrt` / `F2Long` / `M Long`).
Tune the VFO to the network frequency first (151.500 MHz for the MegaNet networks,
FM, narrow or wide) — the app listens on whatever the radio is tuned to.

```
 ALERT MDM        -98dBm SQ *          <- status: input, RSSI, squelch open, capturing
 LOUDOUN BR                            <- station name from the table
 1599              LVL                 <- value (big), sensor kind
                   #6129               <- ALERT address
 6129 LOUDOUN BR     1599              <- last 4 readings
 6130 LOUDOUN BR    13.4V
 6128 LOUDOUN BR      412
```

| Key | Action |
|---|---|
| **EXIT** | leave the app (settings are saved) |
| **MENU** | settings (below) |
| **▲ / ▼** | fine squelch level up / down (0.0 … 9.0), applied immediately |
| **\*** | voice announcements on / off |
| **F (#)** | monitor: un-mute the audio so you hear the bursts |
| **1** | raw view: sync/frame/gated counters and the first bytes of the last capture |
| **5** | say the last reading again |
| **0** | clear history and counters |

Values are shown as MegaNet shows them: battery addresses as volts (raw ÷ 10), rain
as the tip count (MegaNet multiplies by 0.2 mm per tip unless a station records its
bucket size), water level raw (the scale is per site and not in the database), and
`2047` as `FULL` (over-range / dead sensor).

Voice: the UV-K5 voice ROM only holds digits and a few fixed words, so a reading is
spoken as the address digits followed by the value digits (e.g. "six one two nine —
one five nine nine"). Needs menu `Voice` set to English and voice on (`*`). Unknown
addresses are announced too unless `UNKNOWN` is set to `HIDE`.

Each decoded reading is also written to the UART (the programming cable, 38400 8N1)
as `ALERT,<id>,<value>,<ABF|EIF>,<rssi dBm>,<name>` so a laptop can log it.

### Settings (MENU inside the app)

▲/▼ change the value, MENU / \* move to the next / previous row, EXIT saves.

| Row | Meaning | Default |
|---|---|---|
| `INPUT` | `BK MODEM` = the BK4819 FSK engine as a bit slicer (no hardware change). `ADC PA8` = MCU ADC software demodulator (needs the one-wire mod below). | BK MODEM |
| `POLARITY` | Async framing polarity. `NEG` = start 1 / stop 0 (Australian ALERT hardware), `STD` = start 0 / stop 1, `ANY` tries both. | NEG |
| `VOICE` | speak new readings | ON |
| `SQ GATE` | accept a capture only if the squelch was open during it (kills noise decodes) | ON |
| `UNKNOWN` | show / hide addresses that are not in the station table | SHOW |
| `CONFIRM` | require the same reading twice inside one burst | OFF |
| `MONITOR` | audio un-muted | OFF |
| `MDM MODE` | BK4819 FSK receive mode: `FFSK1218` (1200/1800 Hz tone pair), `FFSK1224`, `SAME` (NOAA SAME demodulator, 1562/2083 Hz), `DIRECT` (carrier FSK) | FFSK1218 |
| `BAUD` | bit clock: 200 / 300 / 600 / 1200 | 300 |
| `SYNC` | the sync word the engine waits for: `0000` / `FFFF` / `AAAA` / `5555`. With an ALERT preamble (idle tone) `0000` or `FFFF` matches the preamble itself, so the capture starts before the data. | 0000 |
| `SYNC LEN` | 2 or 4 sync bytes | 2 |
| `INVERT` | invert the modem data | OFF |
| `BIT REV` | reverse the bit order inside each captured byte | OFF |
| `RX GAIN` | modem RX gain 0–3 | 3 |
| `CAPTURE` | bytes captured per sync (16–240; 96 bytes = 768 bits = 2.5 s at 300 baud) | 96 |
| `SQL LEVEL` | the fine squelch, same as ▲/▼ | — |

### Fine squelch (whole radio, not just the app)

Menu **Sql** is now 0.0 … 9.0 in steps of 0.1 (90 steps instead of 9). Whole levels
1–9 use the radio's own calibration tables exactly as before; the tenths are
interpolated between the neighbouring whole levels, and 0.1–0.9 interpolate between
"wide open" and level 1 — which is where the stock firmware jumps straight from
"static at full volume" to "weak signals cut off". Turn it up from 0.0 until the
noise floor just closes; weak bursts then still open it.

Two new menu items control the BK4819's squelch timing (stock firmware used open
delay 5, close delay 2 and did not expose them):

* **SqOpen** 0–7 — squelch open delay, 0 = fastest. Default 1.
* **SqClos** 0–3 — squelch close delay. Default 2.

Both apply to normal listening as well as to the ALERT app's squelch gate. The
values live in a spare EEPROM block (0x0F48) and survive a factory reset of the
channels.

---

## Building

```sh
sudo apt-get install gcc-arm-none-eabi python3-crcmod   # Ubuntu 22.04 / 24.04
make -j                                                 # -> firmware.packed.bin
make -C test                                            # host unit tests (plain gcc)
```

`make` prints the size; the flash limit is 61,440 bytes and the default build sits a
few hundred bytes under it, so if you enable more egzumer features you will have to
give something up (usually a smaller station table, see below).

## Flashing the handheld

You need: the radio, the **Kenwood-style two-pin USB programming cable** (the same
one CHIRP uses — a "K1" plug: 2.5 mm + 3.5 mm jacks into the side socket under the
rubber flap, USB at the other end, usually a CH340 or PL2303 chip), and a laptop with
**Chrome or Edge** (the web flasher uses Web Serial, which Firefox and Safari do not
have). The whole thing takes about a minute; the radio can be flashed as many times
as you like and cannot be "bricked" by a bad image because the bootloader is in a
separate, protected part of the chip — if a flash goes wrong, just do it again.

### 1. Get the firmware file

Either build it (`make` above → `firmware.packed.bin`), or download it from GitHub:

* **Actions → build → the latest green run → Artifacts** — `quansheng-alert-default-…`
  or `quansheng-alert-constitution-hill-…` (South-East Queensland station table).
  Unzip it; you want the file ending in **`.packed.bin`** (the plain `.bin` is for
  `k5prog` only).
* or **Releases** (created when a `v*` tag is pushed) — the `.packed.bin` is attached.

### 2. Back up the radio first (once)

Still on stock or egzumer firmware, plug the cable in, switch the radio on normally,
open <https://egzumer.github.io/uvtools/>, go to **EEPROM → Backup** (called
*Calibration / EEPROM backup* in some versions), **Connect**, pick the cable's COM
port, and save the file it offers. That file holds the radio's calibration
(squelch tables, TX power, battery); the firmware never writes to those areas, but
having the backup means any experiment is reversible.

### 3. Put the radio into bootloader mode

1. Switch the radio **off**.
2. Plug the programming cable into the radio and the laptop.
3. **Hold PTT** and, still holding it, turn the volume knob to switch the radio
   **on**. The torch LED comes on **white/steady** and the screen stays blank (or
   shows a bootloader version on some units). Let go of PTT.

If the LED does not come on, the radio booted normally — switch off and try again,
pressing PTT before turning the knob.

### 4. Flash

1. Open <https://egzumer.github.io/uvtools/> in Chrome/Edge and choose
   **Firmware flasher**.
2. **Choose file** → select the `.packed.bin` from step 1.
3. **Connect** → pick the cable's serial port from the browser pop-up
   (on Windows it is "USB-SERIAL CH340 (COMx)"; if nothing is listed, install the
   [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html) and re-plug).
4. **Flash**. A progress bar runs for 10–20 s and the page says *done*.
5. Switch the radio off and on. The boot screen shows the build tag (`ALERTRX` and
   the git hash). Your channels and settings are untouched — only the firmware is
   replaced.

Command-line alternative (Linux/macOS), with the radio in bootloader mode:

```sh
git clone https://github.com/sq5bpf/k5prog && make -C k5prog
k5prog/k5prog -F -YYY -b firmware.bin           # note: the plain .bin, not .packed.bin
```

### 5. First-time settings

* Menu **Voice** → English (needed for the spoken readings).
* Menu **Sql** → start at 0.0 and raise it until the static just stops.
* Optionally menu **F1Long** (or F2Long / M Long) → `ALERT RX`, so one long press of
  the side button opens the receiver; F then 0 always works too.
* Tune VFO A to **151.500** FM (narrow), then F 0.

### Going back

Flash any other `.packed.bin` (stock Quansheng 2.01.xx, egzumer, …) the same way.
The fine-squelch and ALERT settings live in an EEPROM block no other firmware uses,
so they are ignored, not harmful, and come back when this firmware is reflashed.

### Which radio is it?

This build is for the original **DP32G030 + BK4819** hardware: UV-K5, UV-K5(8), UV-K6,
UV-5R Plus — the one in the photo with the `M`/`A` … `EXIT`/`D` keypad. The newer
**UV-K5 "V3" / UV-K1** use a different MCU (PY32F071) and need a different firmware
line; the web flasher refuses the image on those, nothing is damaged.

GitHub Actions (`.github/workflows/build.yml`) builds on every push, weekly, and on
demand; the packed firmware is an artifact of every run and a release asset for
`v*` tags. The weekly run also regenerates the station table from MegaNet and
commits it back if it changed, so the radio's station list follows the database
without anyone editing this repo.

## The station table

`tools/gen_stations.py` reads three files straight from the MegaNet repository
(`stations.json`, `data/All 2021 Working 2.txt`, `data/ALL_REPEATERS.csv`) —
locally with `--meganet-dir ../MegaNet` or over the network from
`cdomotor-g/MegaNet@main` — applies `stations.filter`, and writes
`app/alert_stations_gen.h`. Nothing from MegaNet is copied into this repo other than
that generated header, which is a build artefact; MegaNet stays the single source.

The whole database is about 44 KB of names — the radio has room for roughly 6 KB —
so `stations.filter` selects which networks are baked in. It ships with the biggest
network(s) that fit; run

```sh
python3 tools/gen_stations.py --meganet-dir ../MegaNet --list-networks
```

to see the cost of every network and catchment, edit the filter (`network`,
`catchment`, `range`, `station`, `id`, `exclude …` directives are all documented in
the file), regenerate, rebuild. CI builds two variants from the same source:
`default` (`stations.filter`: Mt Kanigan, Mt Glorious, Barcaldine and a few
singletons) and `constitution-hill` (`filters/constitution-hill.filter`: the
South-East Queensland network, built with `ENABLE_FLASHLIGHT=0 ENABLE_RSSI_BAR=0`
because it needs 6.8 KB of table). Both packed images are artefacts of every run. Addresses outside the table still decode and display
(as `ID nnnn NOT IN TABLE`); only the name is missing.

Sites are stored as a base address plus a 3-bit sensor kind for up to five
consecutive addresses (rain / level / battery / repeater / other / check), which is
how ALERT addresses are allocated in practice, and the lookup is a binary search in
flash. ALERT addresses are only unique per region (about 600 are reused across
Australia), which is another reason the table is regional.

## How the receiver works

**ALERT (ERTS) format.** A reading is four 10-bit asynchronous words at 300 baud —
start bit, 8 data bits LSB first, stop bit — carrying a 13-bit address and an 11-bit
value plus 8 fixed check bits (ABF) or a 6-bit CRC (EIF). Australian hardware uses
negative logic (start = 1, stop = 0). `app/alert_decode.c` is a straight port of
MegaNet's `packets.js` codec with a UART-style bit scanner in front of it that finds
frames at any bit offset in a captured bit stream, tries both polarities if asked,
and never joins words separated by more than 20 idle bits. Because ABF only has 8
check bits per frame, roughly 1 random 40-bit window in 256 looks valid, so the app
additionally gates captures on the squelch, prefers the negative polarity, and can
demand two copies of a reading (`CONFIRM`).

**Input 1 — BK4819 FSK engine ("BK MODEM").** The BK4819 has a packet modem meant
for 1200-baud FFSK messaging; it cannot demodulate Bell-202 300-baud AFSK as a
packet, but it can be used as a bit slicer: its 1200/1800 Hz tone discriminator
still separates 1200 Hz from 2200 Hz, its bit clock (`REG_72`) is set to 300 baud,
and its sync word is set to the idle pattern so every burst is dumped raw into the
FIFO, where the software framer takes over. This is the same trick the TA1JS APRS
firmware uses to receive 1200-baud AX.25. Whether the chip's clock recovery holds at
300 baud is the one thing only a field test can answer, hence every modem parameter
is on the settings screen and the raw view shows what came in.

**Input 2 — MCU ADC ("ADC PA8").** The proven way to decode odd FSK on this radio
(dkoryto's RS41 radiosonde firmware) is to sample the discriminator audio with the
MCU's SAR ADC. This firmware includes a 9600 Hz sampler driven from SysTick, DC
removal, two 16-sample quadrature correlators at 1200 Hz and 2200 Hz, a bit-clock
DPLL and the same framer. It needs a one-wire hardware modification:

> **BK4819 pin 8 (EARO, the audio output) → 100 nF capacitor → DP32G030 pin 9
> (PA8, ADC channel 3).** PA8 is also UART1 RX (the programming cable), which the app
> reconfigures as an ADC input only while `ADC PA8` is selected and restores on exit,
> so programming still works.

## First field test

1. Flash, set the VFO to 151.500 MHz FM, set `Sql` so the static just closes.
2. `F` `0` to open ALERT RX. The status line shows RSSI and `SQ` when the squelch is
   open; `*` appears while a capture is running.
3. Press `1` for the raw view and wait for a station transmission (a short
   two-tone burst). `SYNC` should count up when a burst arrives. If it never does,
   try `SYNC` `FFFF` (idle tone is the other Bell-202 tone), then `MDM MODE` `SAME`.
4. If `SYNC` counts but `FRM` stays at zero, look at the 20 bits shown: a good
   capture has long runs of the idle level with 10-bit words in it. Try `INVERT`,
   then `BIT REV`, then `POLARITY ANY`. `RX GAIN` 0–2 helps with strong signals.
5. If the modem path cannot be made to lock at 300 baud, fit the capacitor and select
   `INPUT` `ADC PA8` — the software demodulator is independent of the modem.
6. Once it decodes, turn `SQ GATE` back on if you turned it off, and set `VOICE`.

Please record what worked (mode, sync, invert, polarity) so the defaults can be
fixed in `ALERT_LoadConfig()`.

## Not covered (yet)

* **ALERT2** (AirLink, 4800 bps FSK with Reed–Solomon and convolutional FEC) is out
  of scope for the BK4819 modem; the ADC path could carry it later. The decoder
  already understands the ALERT2 "concentration" record (`ALERT_DecodeA2C`) so an
  ALERT2 receiver's serial output could be relayed through the same display code.
* Rain totals in millimetres need the per-station bucket size, which MegaNet does not
  hold either; water level scaling is per site.

## Layout

```
app/alert.c            the app: modem/ADC capture, screens, settings, voice, UART
app/alert_decode.c     pure decoder (ABF / EIF / A2C), host-testable
app/alert_stations.c   flash table lookup
app/alert_stations_gen.h   GENERATED from MegaNet — do not edit
tools/gen_stations.py  generator (stdlib only) + tools/test_gen_stations.py
stations.filter        which networks go into the table
test/                  host unit tests: make -C test
radio.c, driver/bk4819.c, app/menu.c, ui/menu.c, settings.c   fine squelch + menus
```
