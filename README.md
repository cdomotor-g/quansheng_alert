# quansheng_alert — ALERT telemetry receiver firmware for the Quansheng UV-K5

> **Got a UV-K5 “V3” or a UV-K1?** This repository is not for it. Those use a
> **PY32F071** MCU and report a `7.x` bootloader; everything here is built for
> the **DP32G030**. Go to
> **[quansheng_alert_v3](https://github.com/cdomotor-g/quansheng_alert_v3)**
> instead — the same ALERT receiver, ported onto the
> [F4HWN V3 port](https://github.com/armel/uv-k1-k5v3-firmware-custom).
>
> Take the battery off and read the label: a V3 says `V3` beside the barcode and
> is often model `UV-K5(99)`. The installer here checks the bootloader version
> and refuses to flash a `7.x` radio.

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

### → [Install it on your radio](https://cdomotor-g.github.io/quansheng_alert/)

One page in Chrome or Edge, four steps, nothing to download or compile. It backs your
radio up before it changes anything. See [Installing it on the radio](#installing-it-on-the-radio).

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

The boot screen shows `ALERTRX` and a seven-character build id. Locally that is the
git commit; the published builds instead use `tools/srcid.sh`, a hash of everything
the firmware is compiled from. That makes a build reproducible — the same sources
always give a byte-identical image — so editing documentation does not silently
produce a "new" firmware, and the id on your radio identifies the code it is running
rather than whichever commit happened to trigger the build.

GitHub Actions (`.github/workflows/build.yml`) builds both variants on every push,
weekly and on demand, runs every test, and publishes the results into
`docs/firmware/` so the web installer always offers a current build. The weekly run
also regenerates the station table from MegaNet and commits it back if it changed, so
the radio's station list follows the database without anyone editing this repo.
Pushing a `v*` tag additionally attaches both packed images to a release.

## Installing it on the radio

**→ [Open the installer](https://cdomotor-g.github.io/quansheng_alert/)** — one page, four
steps, nothing to download.

It runs in Chrome or Edge on a computer, talks to the radio over the programming
cable, and does the whole job itself: it carries the firmware, backs your radio up
before touching it, waits for you to put the radio in bootloader mode, writes the
firmware and then confirms what the radio is running afterwards. You need:

* the radio and a **two-pin programming cable** (the Kenwood "K1" type, the same one
  Baofeng radios and CHIRP use: a 3.5 mm and a 2.5 mm plug at one end, USB at the
  other), and
* **Chrome or Edge on a desktop or laptop** — the installer needs Web Serial, which
  Firefox, Safari and every mobile browser lack.

The four steps are: pick a build, connect, back up, install. Each screen explains
what it is doing and what to do if it goes wrong, and the installer refuses to send
anything it cannot verify first.

> **The radio's own USB-C socket cannot be used for this.** On the UV-K5(8) and UV-K6
> it is wired to the charging circuit only — its data pins do not reach the radio's
> serial lines — so no tool can flash or program through it: not this installer, not
> the official Quansheng software, not CHIRP, not `k5prog`. The two-pin cable is the
> only way in. Many kits include one alongside the charging cable, so check the box
> before buying; if you do buy one, prefer a **CH340** cable, because the cheap
> "PL2303" ones are frequently counterfeit chips that current Windows drivers refuse
> to run. If you would rather build one, the pinout and a measure-first procedure are in
> [docs/cable/](docs/cable/index.html) (published at
> [/cable/](https://cdomotor-g.github.io/quansheng_alert/cable/)).

**Nothing here can permanently damage the radio.** The bootloader lives in a separate
part of the chip that is never written, so an interrupted flash leaves the radio in
bootloader mode and you simply run the installer again.

### If the installer says the radio did not answer

Two things look like a baud-rate problem and are not. A radio switched on normally
says nothing until asked, so silence is expected; and a radio in bootloader mode sends
binary, scrambled packets, so what a terminal shows at 38400 is gibberish *when
everything is working*. The rate is fixed at 38400 8N1 in the radio and in every tool.

`tools/k5diag.py` settles it from the command line (`pip install pyserial` first):

```sh
python tools/k5diag.py --port COM3           # listen, then hello, then a baud scan if needed
python tools/k5diag.py --port COM3 listen    # radio in bootloader mode: counts its beacons
python tools/k5diag.py --port COM3 hello     # radio on normally: reads its firmware version
```

It reads the bootloader's beacons and checks their checksums, so "beacons at 38400"
proves the cable, the polarity and the rate in one go; the remaining suspects are then
on the computer side — most often another program (a terminal, CHIRP, a second
browser tab) still holding the port. The installer's **Toolbox → Check the cable**
runs the same sequence in the browser, no Python needed. Wiring faults are covered in
[docs/cable/](docs/cable/index.html#diagnose).

### What "back up" does and does not cover

The installer's backup step saves your radio's **EEPROM**: the factory calibration,
every channel and all your settings — the part that is unique to your radio and
cannot be downloaded from anywhere. It is saved to your Downloads folder and a copy
is kept in the browser. The **Toolbox → Restore a backup** section writes it back.

The firmware itself **cannot be backed up**: the bootloader has no read command, so
no tool — this one, the official one, or `k5prog` — can read a radio's firmware out.
That is not a problem in practice, because firmware is replaceable in a minute from
the installer or from a stock image, whereas calibration is not.

### Going back to stock, or to another firmware

Use **Advanced → Use my own firmware file instead** in step 1 and pick any
`.packed.bin` (stock Quansheng, egzumer, anything else). The fine-squelch and ALERT
settings live in an EEPROM block no other firmware touches, so they are ignored by
other builds and come back if you return to this one.

### Which radio is this for?

The original **DP32G030 + BK4819** hardware: UV-K5, UV-K5(8), UV-K6, UV-5R Plus — the
one with the `M`/`A` … `EXIT`/`D` keypad.

> [!WARNING]
> **Not for the UV-K5 “V3” or UV-K1.** Those use a **PY32F071** microcontroller, not
> the DP32G030. This firmware cannot run on them: installing it leaves a radio that
> will not start. Use
> [armel/uv-k1-k5v3-firmware-custom](https://github.com/armel/uv-k1-k5v3-firmware-custom)
> instead.
>
> **How to tell:** take the battery off and read the label.
>
> | | V1 — supported | V3 — **not** supported |
> |---|---|---|
> | Label | no version marking | **`V3`** printed beside the barcode |
> | Model | `UV-K5`, `UV-K5(8)`, `UV-K6`, `UV-5R Plus` | often `UV-K5(99)` |
> | Bootloader version | `2.x` | `7.x` |
> | Stock firmware | `2.01.26` and similar | `7.x` |
>
> A V3 speaks the same serial protocol as a V1 — it will connect, answer, and back up
> perfectly normally — which is exactly what makes it look flashable. The version
> numbers are the giveaway. The installer now reads the bootloader version out of the
> radio's own broadcast and refuses to flash anything that is not `2.x`; an unrecognised
> version warns instead of blocking, and either can be overridden deliberately.

Nothing here can damage a V3 — the installer stops before writing, and even an
overridden install leaves the bootloader intact, so the radio still enters bootloader
mode and can be re-flashed with firmware that suits it.

<details>
<summary>Doing it by hand instead (or from Linux without a browser)</summary>

Download `quansheng-alert-default.packed.bin` from
[docs/firmware/](docs/firmware/) or from a release, then either use
[egzumer's uvtools](https://egzumer.github.io/uvtools/), or with the radio in
bootloader mode:

```sh
git clone https://github.com/sq5bpf/k5prog && make -C k5prog
k5prog/k5prog -F -YYY -b docs/firmware/quansheng-alert-default.bin   # the raw .bin, not .packed.bin
```

Bootloader mode is: radio off, hold PTT, switch on — the torch LED glows white and
the screen stays blank.
</details>

### After the first install

1. **Menu → Voice → English**, so it reads readings out.
2. **Menu → Sql**: from `0.0` upwards until the static just goes quiet.
3. Tune to **151.500 MHz**, FM.
4. **F** then **0** opens the ALERT receiver.

## The installer, and hosting it

The installer is a static page in [`docs/`](docs/) — no build step, no frameworks, no
external requests. `docs/js/k5protocol.js` is the wire protocol, `docs/js/k5radio.js`
drives Web Serial, `docs/js/app.js` is the wizard. The firmware it offers lives in
`docs/firmware/` with a `manifest.json`, both written by `tools/publish_firmware.py`
and refreshed by CI, which is why the page always offers a current build.

It is a fresh implementation rather than a fork: the protocol is documented by
[k5prog](https://github.com/sq5bpf/k5prog) and
[uvmod](https://github.com/whosmatt/uvmod)/[uvtools](https://github.com/egzumer/uvtools),
but none of their code is used here. What it adds over those tools is the guided
flow, a backup that happens by default instead of being a separate page, firmware
served with the tool so there is nothing to find and download, per-block retries,
image validation before anything is written, automatic detection of bootloader mode,
verification after the reboot, and an EEPROM restore.

**To publish it (one-time, repository owner):** GitHub → **Settings** → **Pages** →
under *Build and deployment* set **Source: Deploy from a branch**, **Branch: `main`**,
**Folder: `/docs`** → **Save**. A minute later it is live at
`https://cdomotor-g.github.io/quansheng_alert/`. Nothing else needs configuring, and
every later push updates it.

Testing it without a radio:

```sh
npm install          # playwright, only needed for the browser test
npm test             # protocol vectors, then the whole wizard against a simulated radio
```

`test/test_installer.mjs` injects a fake serial port that speaks the real protocol,
then drives the page from picking a build through to the post-reboot check, and
compares the bytes the "radio" received against the real firmware image.


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
test/                  host unit tests: make -C test, npm test
docs/                  the web installer served by GitHub Pages
docs/js/k5protocol.js  UV-K5 wire protocol (framing, obfuscation, CRC, commands)
docs/js/k5radio.js     Web Serial transport: backup, restore, flash
docs/js/app.js         the four-step wizard
docs/firmware/         published builds + manifest.json, written by CI
tools/publish_firmware.py  copies a build into docs/firmware and updates the manifest
radio.c, driver/bk4819.c, app/menu.c, ui/menu.c, settings.c   fine squelch + menus
```
