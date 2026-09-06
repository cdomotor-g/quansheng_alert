#!/usr/bin/env python3
"""tools/k5diag.py must speak exactly the protocol docs/js/k5protocol.js does.

The reference packets below were produced by the JavaScript (`node -e` over
K5.frame / K5.cmd); if node is on the path they are regenerated live so the two
cannot drift apart unnoticed.  Run:  python3 test/test_k5diag.py
"""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import k5diag  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..')

REF = {
    'hello':   'ab cd 08 00 02 69 10 e6 44 a8 5a 24 b9 a9 dc ba',
    'version': 'ab cd 14 00 26 69 04 e6 04 91 0d 40 21 35 d5 40 13 03 e9 80 16 6c 14 e6 dc 5f dc ba',
    'beacon':  'ab cd 24 00 0e 69 34 e6 2f 93 0f 46 3d 66 85 0b 5f 5e eb 41 34 eb 67 3d 1c bf 3d 70 '
               '0f 05 e3 40 13 03 e9 80 16 6c 14 e6 51 7f dc ba',
}
BEACON_PAYLOAD = bytes([0x18, 0x05, 0x20, 0x00, 0x01, 0x02, 0x02, 0x06, 0x1c, 0x53, 0x50, 0x4b, 0x4c, 0x5d,
                        0x02, 0xc1, 0x22, 0x87, 0x73, 0xdb, 0x32, 0x2e, 0x30, 0x30, 0x2e, 0x30, 0x36, 0x00,
                        0, 0, 0, 0, 0, 0, 0, 0])
VERSION_PAYLOAD = bytes([0x30, 0x05, 0x10, 0x00, 0x2a]) + bytes(15)


def from_js():
    """Ask the JavaScript for the same three packets, if node is available."""
    if not shutil.which('node'):
        return None
    js = '''
const K5 = require("./docs/js/k5protocol.js");
const hex = b => Array.from(b, x => x.toString(16).padStart(2, "0")).join(" ");
const v = new Uint8Array(16); v[0] = 0x2a;
const beacon = Uint8Array.from(%s);
console.log(JSON.stringify({hello: hex(K5.frame(K5.cmd.hello())),
  version: hex(K5.frame(K5.cmd.version(v))), beacon: hex(K5.frame(beacon))}));
''' % list(BEACON_PAYLOAD)
    out = subprocess.run(['node', '-e', js], cwd=ROOT, capture_output=True, text=True, check=True)
    import json
    return json.loads(out.stdout)


def hx(b):
    return ' '.join(f'{x:02x}' for x in b)


def main():
    ref = from_js() or REF
    source = 'live JavaScript' if ref is not REF else 'stored vectors'
    fails = 0

    def check(name, cond, detail=''):
        nonlocal fails
        print(('ok   ' if cond else 'FAIL ') + name + (f'  {detail}' if detail and not cond else ''))
        fails += 0 if cond else 1

    check('hello frame matches JS', hx(k5diag.frame(k5diag.cmd_hello())) == ref['hello'],
          hx(k5diag.frame(k5diag.cmd_hello())))
    check('version frame matches JS', hx(k5diag.frame(VERSION_PAYLOAD)) == ref['version'])
    check('beacon frame matches JS', hx(k5diag.frame(BEACON_PAYLOAD)) == ref['beacon'])

    pkts, rest = k5diag.deframe(bytes.fromhex(ref['beacon'].replace(' ', '')))
    check('deframe recovers the beacon', pkts == [(BEACON_PAYLOAD, True)] and rest == b'')
    check('beacon is recognised', k5diag.is_bootloader_beacon(BEACON_PAYLOAD))
    check('bootloader version is read out', k5diag.ascii_run(BEACON_PAYLOAD[7:]) == '2.00.06')

    noisy = b'\x00\xffab' + bytes.fromhex(ref['hello'].replace(' ', '')) + b'\xab\xcd\x03'
    pkts, rest = k5diag.deframe(noisy)
    check('deframe skips junk and keeps a partial tail',
          pkts == [(k5diag.cmd_hello(), True)] and rest == b'\xab\xcd\x03')

    damaged = bytearray(bytes.fromhex(ref['hello'].replace(' ', '')))
    damaged[6] ^= 0x01
    pkts, _ = k5diag.deframe(bytes(damaged))
    check('a corrupted packet fails its checksum', len(pkts) == 1 and pkts[0][1] is False)

    check('crc16 xmodem check value', k5diag.crc16(b'123456789') == 0x31c3)

    print(f'{"all passed" if not fails else f"{fails} failed"} (reference: {source})')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
