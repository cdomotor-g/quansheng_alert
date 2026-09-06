#!/usr/bin/env python3
"""Serial fault-finder for a Quansheng UV-K5 on a programming cable.

Answers the questions people actually have when the installer says "the radio
did not answer":

  * is the adapter even visible, and which port is it?
  * does the adapter work at all? (loopback)
  * is anything arriving from the radio, and is it at 38400 baud?
  * is that "gibberish" in my terminal the bootloader talking, or noise?
  * will the radio answer a hello when switched on normally?

The bootloader's chatter is binary, XOR-obfuscated packets, so at the RIGHT
baud it still looks like garbage in a terminal.  The only reliable test is
whether the bytes frame up as  AB CD .. DC BA  packets with a good checksum,
which is what this script checks.  It never writes to the radio's flash or
EEPROM: the only thing it ever sends is the harmless "hello" the installer
uses to read the firmware version.

    pip install pyserial
    python k5diag.py                       # find the port, then run every check
    python k5diag.py --port COM3           # same, on a named port
    python k5diag.py --port COM3 listen    # just watch the line for a few seconds
    python k5diag.py --port COM3 scan      # try every plausible baud rate
    python k5diag.py --port COM3 hello     # ask the firmware for its version
    python k5diag.py --port COM3 loopback  # adapter TX shorted to RX
    python k5diag.py ports                 # list serial ports

The protocol is the same one docs/js/k5protocol.js implements; the two are
kept byte-for-byte compatible (see test/test_k5diag.py).
"""

import argparse
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover - a message beats a traceback
    sys.exit("This needs pyserial:  pip install pyserial")

# ---------------------------------------------------------------- protocol --

RADIO_BAUD = 38400

XOR_KEY = bytes([0x16, 0x6c, 0x14, 0xe6, 0x2e, 0x91, 0x0d, 0x40,
                 0x21, 0x35, 0xd5, 0x40, 0x13, 0x03, 0xe9, 0x80])

SESSION = bytes([0x6a, 0x39, 0x57, 0x64])   # the token k5prog has always used


def xor(data):
    return bytes(b ^ XOR_KEY[i % len(XOR_KEY)] for i, b in enumerate(data))


def crc16(data, crc=0):
    """CRC-16/XMODEM: poly 0x1021, init 0, no reflection, no final xor."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xffff
    return crc


def frame(payload):
    """AB CD | len | XOR(payload + crc16) | DC BA"""
    body = xor(payload + crc16(payload).to_bytes(2, 'little'))
    return b'\xab\xcd' + len(payload).to_bytes(2, 'little') + body + b'\xdc\xba'


def deframe(buf):
    """Pull complete packets out of a byte buffer.

    Returns (packets, rest) where each packet is (payload, crc_ok). Junk in
    front of a packet is skipped a byte at a time, so the stream re-syncs by
    itself after noise - the same rule as the installer.
    """
    packets = []
    at = 0
    while True:
        while at + 1 < len(buf) and not (buf[at] == 0xab and buf[at + 1] == 0xcd):
            at += 1
        if at + 8 > len(buf):
            break
        length = buf[at + 2] | (buf[at + 3] << 8)
        total = length + 8
        if length > 0x400:
            at += 1
            continue
        if at + total > len(buf):
            break
        if buf[at + total - 2] != 0xdc or buf[at + total - 1] != 0xba:
            at += 1
            continue
        body = xor(buf[at + 4:at + total - 2])
        payload = body[:length]
        want = body[length] | (body[length + 1] << 8)
        packets.append((payload, crc16(payload) == want))
        at += total
    return packets, buf[at:]


def cmd_hello():
    return bytes([0x14, 0x05, 0x04, 0x00]) + SESSION


def is_bootloader_beacon(p):
    return (p[0] == 0x18 and len(p) >= 7 and p[2] == 0x20 and p[3] == 0x00
            and p[4] == 0x01 and p[5] == 0x02 and p[6] == 0x02)


def ascii_run(data, minimum=4):
    """The longest printable run in a payload - the bootloader's version
    string ("2.00.06") sits inside its beacon, the firmware's in its hello reply."""
    best, cur = '', ''
    for b in data:
        if 0x20 <= b <= 0x7e:
            cur += chr(b)
        else:
            if len(cur) > len(best):
                best = cur
            cur = ''
    if len(cur) > len(best):
        best = cur
    return best if len(best) >= minimum else ''


# ------------------------------------------------------------------ output --

def hexdump(data, limit=96):
    shown = data[:limit]
    lines = []
    for i in range(0, len(shown), 16):
        chunk = shown[i:i + 16]
        hexes = ' '.join(f'{b:02x}' for b in chunk)
        text = ''.join(chr(b) if 0x20 <= b <= 0x7e else '.' for b in chunk)
        lines.append(f'    {i:04x}  {hexes:<47}  {text}')
    if len(data) > limit:
        lines.append(f'    ... {len(data) - limit} more bytes')
    return '\n'.join(lines)


def say(msg=''):
    print(msg, flush=True)


def verdict(msg):
    say()
    say('=> ' + msg)


# ------------------------------------------------------------------- ports --

KNOWN_CHIPS = {
    (0x1a86, 0x7523): 'CH340 (the usual K5 cable chip)',
    (0x1a86, 0x55d4): 'CH9102',
    (0x10c4, 0xea60): 'CP210x',
    (0x067b, 0x2303): 'PL2303 (often counterfeit - current Windows drivers may refuse it)',
    (0x067b, 0x23a3): 'PL2303GC',
    (0x0403, 0x6001): 'FTDI FT232',
}


def list_ports():
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
    if not ports:
        say('No serial ports found. If the cable is plugged in, its driver is missing: '
            'install the CH340 driver (or the one for your chip) and re-plug it.')
        return ports
    say('Serial ports:')
    for p in ports:
        chip = KNOWN_CHIPS.get((p.vid or 0, p.pid or 0))
        ident = f'{p.vid:04x}:{p.pid:04x}' if p.vid else 'no USB id'
        say(f'  {p.device:<14} {p.description}  [{ident}]' + (f'  {chip}' if chip else ''))
    return ports


def pick_port(name):
    if name:
        return name
    ports = list_ports()
    usb = [p for p in ports if p.vid]
    if len(usb) == 1:
        say(f'Using {usb[0].device}.')
        return usb[0].device
    if not ports:
        sys.exit(1)
    sys.exit('More than one port - say which with --port COMx')


def open_port(name, baud):
    try:
        return serial.Serial(name, baud, bytesize=8, parity='N', stopbits=1, timeout=0.05)
    except serial.SerialException as err:
        sys.exit(f'Could not open {name}: {err}\n'
                 'On Windows that usually means another program (a terminal, the installer '
                 'tab, CHIRP) still has the port open. Close it and try again.')


# ------------------------------------------------------------------ checks --

def listen(port, seconds, quiet=False):
    """Read for `seconds` and return a summary dict."""
    port.reset_input_buffer()
    raw = bytearray()
    beacon_times = []
    good = bad = 0
    versions = set()
    pending = b''
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            chunk = port.read(512)
        except serial.SerialException as err:
            sys.exit(f'The port went away while listening ({err}). '
                     'Was the cable unplugged, or did another program grab the port?')
        if not chunk:
            continue
        raw += chunk
        packets, pending = deframe(pending + chunk)
        for payload, ok in packets:
            if not ok:
                bad += 1
                continue
            good += 1
            if is_bootloader_beacon(payload):
                beacon_times.append(time.monotonic())
                v = ascii_run(payload[7:])
                if v:
                    versions.add(v)
        if len(pending) > 4096:
            pending = pending[-4096:]

    gaps = [b - a for a, b in zip(beacon_times, beacon_times[1:])]
    summary = {
        'bytes': len(raw), 'raw': bytes(raw), 'good': good, 'bad': bad,
        'beacons': len(beacon_times),
        'interval': (sum(gaps) / len(gaps)) if gaps else None,
        'versions': sorted(versions),
    }
    if not quiet:
        say(f'  {len(raw)} bytes in {seconds:g} s: {good} good packets, {bad} with a bad checksum, '
            f'{len(beacon_times)} bootloader beacons')
        if raw:
            say(hexdump(raw))
    return summary


def check_listen(port, seconds):
    say(f'Listening on {port.name} at {port.baudrate} baud for {seconds:g} s ...')
    s = listen(port, seconds)
    if s['beacons']:
        iv = f', one every {s["interval"]:.2f} s' if s['interval'] else ''
        ver = f' Bootloader version {", ".join(s["versions"])}.' if s['versions'] else ''
        verdict(f'The radio is in BOOTLOADER MODE and the line is good: {s["beacons"]} beacons{iv}.{ver} '
                'Baud rate, wiring and polarity are all right. If the installer still cannot flash, '
                'the problem is on the computer side: another program holding the port, or the '
                'browser not being given this port.')
    elif s['good']:
        verdict('Framed packets are arriving but they are not bootloader beacons. The radio is '
                'probably running normal firmware (the ALERT receiver writes CSV, not packets, '
                'so this is unusual - re-run with "hello").')
    elif s['bytes']:
        verdict('Bytes are arriving but none of them frame up as AB CD .. DC BA packets. That is '
                'NOT a baud-rate mismatch by itself - at the right baud the packets are still '
                'unreadable in a terminal. Run "scan" to test other rates; if no rate frames, '
                'suspect an inverted line, a plug not fully home, or a 5 V adapter.')
    else:
        verdict('Nothing at all arrived. Either the radio is switched on normally (it says '
                'nothing unless asked - try "hello"), or the radio\'s TX is not reaching the '
                'adapter\'s RX: swap TX/RX at the adapter, and check the 2.5 mm plug is fully in.')
    return s


BAUDS = [38400, 115200, 57600, 19200, 9600, 76800, 128000, 230400, 4800, 460800]


def check_scan(name, seconds):
    say(f'Trying each baud rate on {name} for {seconds:g} s. Put the radio in bootloader mode '
        '(off, hold PTT, on) so it has something to say.')
    say()
    say(f'  {"baud":>7}  {"bytes":>6}  {"packets":>7}  {"beacons":>7}')
    results = {}
    for baud in BAUDS:
        try:
            port = serial.Serial(name, baud, bytesize=8, parity='N', stopbits=1, timeout=0.05)
        except serial.SerialException as err:
            say(f'  {baud:>7}  cannot open: {err}')
            continue
        with port:
            s = listen(port, seconds, quiet=True)
        results[baud] = s
        say(f'  {baud:>7}  {s["bytes"]:>6}  {s["good"]:>7}  {s["beacons"]:>7}')

    framed = [b for b, s in results.items() if s['good']]
    noisy = [b for b, s in results.items() if s['bytes']]
    if RADIO_BAUD in framed:
        verdict('Valid packets at 38400 - the standard rate - and nothing needs changing. '
                '(Other rates may show a few bytes too; that is the same signal mis-sampled.)')
    elif framed:
        verdict(f'Valid packets only at {framed[0]} baud, which no known UV-K5 bootloader uses. '
                'Check the adapter\'s driver is not applying a divisor, and that this is really '
                'a UV-K5 (the V3/UV-K1 is a different chip).')
    elif noisy:
        verdict('Bytes at some rates but no valid packet at any of them. A baud mismatch would '
                'have shown up above, so look elsewhere: inverted signal (some adapters and '
                'cables invert), plugs not fully home, or a 5 V adapter dragging the line.')
    else:
        verdict('Silence at every rate. The radio\'s TX is not reaching the adapter: wrong '
                'contact on the 2.5 mm plug, TX/RX swapped, or the radio is not actually '
                'in bootloader mode (white torch LED, blank screen).')
    return results


def check_hello(port, attempts=3):
    say(f'Asking the radio on {port.name} for its firmware version ...')
    for i in range(attempts):
        port.reset_input_buffer()
        port.write(frame(cmd_hello()))
        port.flush()
        pending = b''
        end = time.monotonic() + 1.5
        while time.monotonic() < end:
            chunk = port.read(256)
            if not chunk:
                continue
            packets, pending = deframe(pending + chunk)
            for payload, ok in packets:
                if not ok or payload[0] != 0x18:
                    continue
                if is_bootloader_beacon(payload):
                    verdict('The radio is in bootloader mode, which cannot report a version. '
                            'Switch it off and on normally and ask again.')
                    return None
                version = payload[4:20].split(b'\0')[0].decode('ascii', 'replace')
                verdict(f'The radio answered: it is running "{version}". The cable works in '
                        'both directions and the baud rate is right.')
                return version
        say(f'  no reply (attempt {i + 1} of {attempts})')
    verdict('No reply to hello. If "listen" showed bootloader beacons, the radio is just not '
            'in normal mode. Otherwise the adapter\'s TX is not reaching the radio: check '
            'the 3.5 mm sleeve wiring and the series resistor, and that the adapter is 3.3 V.')
    return None


def check_loopback(port):
    say(f'Loopback test on {port.name}: short the adapter\'s TX and RX pins together.')
    probe = b'K5DIAG-' + str(int(time.time())).encode()
    port.reset_input_buffer()
    port.write(probe)
    port.flush()
    got = b''
    end = time.monotonic() + 1.0
    while time.monotonic() < end and len(got) < len(probe):
        got += port.read(len(probe) - len(got))
    if got == probe:
        verdict('The adapter echoed the test string: adapter and driver are fine at this baud.')
        return True
    if got:
        verdict(f'Garbled echo ({got!r}): the adapter is talking to itself but corrupting data. '
                'Try another USB port or cable; a counterfeit PL2303 does this.')
    else:
        verdict('No echo. Either TX and RX are not shorted, or the adapter/driver is dead. '
                'Nothing about the radio can be tested until this passes.')
    return False


def check_auto(name, seconds):
    say('1. Listening for the radio ...')
    with open_port(name, RADIO_BAUD) as port:
        s = check_listen(port, seconds)
        if s['beacons']:
            return
        say()
        say('2. Trying a hello, in case the radio is switched on normally ...')
        if check_hello(port):
            return
    if s['bytes']:
        say()
        say('3. Bytes arrived that did not frame, so scanning baud rates ...')
        check_scan(name, 2.0)
    else:
        say()
        say('Nothing came back either way. Put the radio in bootloader mode (off, hold PTT, '
            'switch on: white torch LED, blank screen) and run "listen"; if that stays silent, '
            'run "loopback" with the adapter\'s TX and RX shorted to rule the adapter out.')


# -------------------------------------------------------------------- main --

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 1)[1])
    ap.add_argument('check', nargs='?', default='auto',
                    choices=['auto', 'ports', 'listen', 'scan', 'hello', 'loopback'])
    ap.add_argument('--port', '-p', help='COM3, /dev/ttyUSB0, ... (auto-detected if only one)')
    ap.add_argument('--baud', '-b', type=int, default=RADIO_BAUD,
                    help=f'rate for listen/hello/loopback (default {RADIO_BAUD})')
    ap.add_argument('--seconds', '-s', type=float, default=4.0,
                    help='how long to listen (default 4)')
    args = ap.parse_args(argv)

    if args.check == 'ports':
        list_ports()
        return 0

    name = pick_port(args.port)
    if args.check == 'auto':
        check_auto(name, args.seconds)
    elif args.check == 'scan':
        check_scan(name, min(args.seconds, 3.0))
    else:
        with open_port(name, args.baud) as port:
            if args.check == 'listen':
                check_listen(port, args.seconds)
            elif args.check == 'hello':
                check_hello(port)
            elif args.check == 'loopback':
                check_loopback(port)
    return 0


if __name__ == '__main__':
    sys.exit(main())
