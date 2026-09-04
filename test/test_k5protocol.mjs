// Tests for the installer's protocol layer (docs/js/k5protocol.js).
//   node test/test_k5protocol.mjs
//
// The packet vectors are the byte sequences the two established tools put on
// the wire (k5prog's uvk5_hello, and the same framing as the uvmod/uvtools
// browser flasher), so a mistake in framing, obfuscation or CRC shows up here
// rather than on someone's radio.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

// k5protocol.js is a plain script that defines `const K5`; evaluate it and take
// the binding out of the module scope it creates.
const source = readFileSync(join(root, 'docs/js/k5protocol.js'), 'utf8');
const K5 = new Function(`${source}; return K5;`)();

let failures = 0;
function test(name, fn) {
	try { fn(); console.log(`  ok    ${name}`); }
	catch (err) { failures++; console.log(`  FAIL  ${name}\n        ${err.message}`); }
}

const hex = (b) => Array.from(b, x => x.toString(16).padStart(2, '0')).join(' ');

console.log('k5protocol');

test('CRC-16/XMODEM matches the reference vector', () => {
	// The canonical check value for CRC-16/XMODEM over "123456789" is 0x31C3.
	assert.equal(K5.crc16(new TextEncoder().encode('123456789')), 0x31c3);
});

test('the hello packet is byte-for-byte what k5prog sends', () => {
	// k5prog: uvk5_hello = 14 05 04 00 6a 39 57 64, framed and obfuscated.
	const framed = K5.frame(K5.cmd.hello());
	assert.equal(framed[0], 0xab);
	assert.equal(framed[1], 0xcd);
	assert.equal(framed[2], 8);          // payload length, little endian
	assert.equal(framed[3], 0);
	assert.equal(framed[framed.length - 2], 0xdc);
	assert.equal(framed[framed.length - 1], 0xba);
	assert.equal(framed.length, 8 + 8);  // 4 header + payload + 2 crc + 2 footer

	// De-obfuscating the body must give the payload back plus its CRC.
	const body = framed.slice(4, framed.length - 2).map((b, i) => b ^ K5.XOR_KEY[i % K5.XOR_KEY.length]);
	assert.equal(hex(body.slice(0, 8)), '14 05 04 00 6a 39 57 64');
	const crc = body[8] | (body[9] << 8);
	assert.equal(crc, K5.crc16(body.slice(0, 8)));
});

test('framing and deframing round-trip, including junk and split packets', () => {
	const payloads = [K5.cmd.hello(), K5.cmd.readEeprom(0x1e00, 0x80), K5.cmd.reset()];
	const stream = [];
	stream.push(0x00, 0xff, 0xab);                      // leading noise, incl. a false start
	for (const p of payloads) stream.push(...K5.frame(p));
	stream.push(0xab, 0xcd, 0x99);                      // a truncated packet at the end

	const { packets, rest } = K5.deframe(Uint8Array.from(stream));
	assert.equal(packets.length, 3);
	packets.forEach((pkt, i) => {
		assert.ok(pkt.crcOk, `packet ${i} CRC`);
		assert.equal(hex(pkt.payload), hex(payloads[i]));
	});
	assert.ok(rest.length > 0, 'the partial packet is kept for next time');
});

test('a corrupted packet is reported, not silently accepted', () => {
	const framed = K5.frame(K5.cmd.hello());
	framed[6] ^= 0x01;                                  // flip a bit inside the payload
	const { packets } = K5.deframe(framed);
	assert.equal(packets.length, 1);
	assert.equal(packets[0].crcOk, false);
});

test('EEPROM read and write commands carry address, length and session token', () => {
	const read = K5.cmd.readEeprom(0x1234, 0x80);
	assert.equal(hex(read.slice(0, 8)), '1b 05 08 00 34 12 80 00');
	assert.equal(hex(read.slice(8, 12)), hex(K5.SESSION));

	const data  = Uint8Array.from({ length: 0x10 }, (_, i) => i);
	const write = K5.cmd.writeEeprom(0x0f50, data);
	assert.equal(hex(write.slice(0, 8)), '1d 05 18 00 50 0f 10 01');
	assert.equal(hex(write.slice(8, 12)), hex(K5.SESSION));
	assert.equal(hex(write.slice(12)), hex(data));
});

test('a flash block is 0x110 bytes with the address big-endian', () => {
	const block = new Uint8Array(0x100).fill(0xa5);
	const cmd   = K5.cmd.flashBlock(0x1200, block, 0xee01);
	assert.equal(cmd.length, 16 + 0x100);
	assert.equal(hex(cmd.slice(0, 8)), '19 05 0c 01 8a 8d 9f 1d');
	assert.equal(cmd[8], 0x12);                          // address MSB
	assert.equal(cmd[9], 0x00);                          // address LSB
	assert.equal(cmd[10], 0xef);                         // final block address, rounded up
	assert.equal(cmd[11], 0x00);
	assert.equal(hex(cmd.slice(16)), hex(block));
});

test('a short final block is padded with zeroes', () => {
	const tail = Uint8Array.from([1, 2, 3]);
	const cmd  = K5.cmd.flashBlock(0x0100, tail, 0x103);
	assert.equal(cmd.length, 16 + 0x100);
	assert.equal(hex(cmd.slice(16, 19)), '01 02 03');
	assert.ok(cmd.slice(19).every(b => b === 0));
});

test('an oversized image is refused before anything is sent', () => {
	assert.throws(() => K5.cmd.flashBlock(0, new Uint8Array(0x100), 0xf001), /too large/);
});

// ---- real firmware images --------------------------------------------------

const packedPath = join(root, 'docs/firmware/quansheng-alert-default.packed.bin');
const rawPath    = join(root, 'docs/firmware/quansheng-alert-default.bin');

if (existsSync(packedPath) && existsSync(rawPath)) {
	const packed = new Uint8Array(readFileSync(packedPath));
	const raw    = new Uint8Array(readFileSync(rawPath));

	test('the published packed image unpacks to exactly the raw build', () => {
		const image = K5.readImage(packed);
		assert.equal(image.packed, true, 'recognised as a packed image');
		assert.equal(image.crcOk, true, 'checksum');
		assert.equal(image.raw.length, raw.length, 'unpacked length');
		assert.equal(hex(image.raw.slice(0, 64)), hex(raw.slice(0, 64)));
		assert.ok(image.raw.every((b, i) => b === raw[i]), 'every byte matches the raw build');
	});

	test('the version string in the published image starts with the wildcard', () => {
		const image = K5.readImage(packed);
		const v = K5.versionString(image.version);
		assert.ok(v.startsWith('*'), `version "${v}" must start with * so every bootloader accepts it`);
	});

	test('a raw build is accepted too, and gets the wildcard version', () => {
		const image = K5.readImage(raw);
		assert.equal(image.packed, false);
		assert.equal(K5.versionString(image.version), '*');
		assert.equal(K5.checkImage(image), null);
	});

	test('both published images pass the safety check', () => {
		assert.equal(K5.checkImage(K5.readImage(packed)), null);
		assert.equal(K5.checkImage(K5.readImage(raw)), null);
	});

	test('a damaged image is rejected with a readable reason', () => {
		const broken = Uint8Array.from(packed);
		broken[broken.length - 1] ^= 0xff;              // wreck the checksum
		const why = K5.checkImage(K5.readImage(broken));
		assert.ok(why && /checksum/i.test(why), `expected a checksum complaint, got: ${why}`);

		assert.ok(/too small/.test(K5.checkImage(K5.readImage(new Uint8Array(100)))));
	});
} else {
	console.log('  skip  published firmware checks (docs/firmware not built yet)');
}

console.log(failures ? `\nFAILED: ${failures}` : '\nPASSED');
process.exit(failures ? 1 : 0);
