// Quansheng UV-K5 serial protocol - pure functions, no I/O.
//
// Written for quansheng_alert from the protocol as documented by two prior
// open-source implementations, neither of whose code is copied here:
//   - sq5bpf/k5prog          (C, the original reverse engineering)
//   - whosmatt/uvmod, egzumer/uvtools  (the browser flasher this tool replaces)
//
// Wire format, both normal and bootloader mode, 38400 8N1:
//
//   AB CD | len_lo len_hi | XOR( payload || crc16_xmodem(payload) ) | DC BA
//
// `len` counts the payload only; the two CRC bytes sit inside the obfuscated
// section but outside the length. Every payload starts with a command byte.
//
// The radio does not checksum what it sends: every reply carries 0xFFFF in
// place of a CRC. Only what we transmit is checksummed for real, so a received
// 0xFFFF means "not supplied" rather than "corrupt". See deframe().
//
// Normal mode (radio switched on the usual way, firmware running):
//   0x14 hello            -> 0x15 reply carrying the firmware version at [4..]
//   0x1b read EEPROM      -> 0x1c reply, data at [8..], max 0x80 bytes a time
//   0x1d write EEPROM     -> 0x1e reply
//   0xdd reset the radio   (no reply)
// The four bytes at [4..7] of hello/read/write are a session token; every
// command in one session must carry the same value.
//
// Bootloader mode (hold PTT while switching on):
//   radio broadcasts 0x18 "I am in flash mode" about twice a second
//   0x30 present version  -> 0x18
//   0x19 write 0x100 bytes of flash -> 0x1a
// There is no read-flash command in any known bootloader, which is why the
// firmware on a radio cannot be backed up - only the EEPROM can.

'use strict';

const K5 = (() => {

	// ---- obfuscation and checksum -----------------------------------------

	const XOR_KEY = Uint8Array.of(
		0x16, 0x6c, 0x14, 0xe6, 0x2e, 0x91, 0x0d, 0x40,
		0x21, 0x35, 0xd5, 0x40, 0x13, 0x03, 0xe9, 0x80);

	function xorInPlace(bytes) {
		for (let i = 0; i < bytes.length; i++)
			bytes[i] ^= XOR_KEY[i % XOR_KEY.length];
		return bytes;
	}

	function xorCopy(bytes) {
		return xorInPlace(Uint8Array.from(bytes));
	}

	// CRC-16/XMODEM: poly 0x1021, init 0, no reflection, no final xor.
	function crc16(bytes, crc = 0) {
		for (let i = 0; i < bytes.length; i++) {
			crc ^= bytes[i] << 8;
			for (let bit = 0; bit < 8; bit++)
				crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
			crc &= 0xffff;
		}
		return crc;
	}

	// ---- packet codec ------------------------------------------------------

	const SOF = Uint8Array.of(0xab, 0xcd);
	const EOF = Uint8Array.of(0xdc, 0xba);

	// What the radio puts where a checksum should go.
	const NO_CRC = 0xffff;

	function frame(payload) {
		const sum  = crc16(payload);
		const body = new Uint8Array(payload.length + 2);
		body.set(payload, 0);
		body[payload.length]     = sum & 0xff;
		body[payload.length + 1] = (sum >> 8) & 0xff;
		xorInPlace(body);

		const packet = new Uint8Array(body.length + 6);
		packet.set(SOF, 0);
		packet[2] = payload.length & 0xff;
		packet[3] = (payload.length >> 8) & 0xff;
		packet.set(body, 4);
		packet.set(EOF, packet.length - 2);
		return packet;
	}

	// Pull complete packets out of a rolling receive buffer. Returns
	// { packets: [Uint8Array payload...], rest: Uint8Array }. Malformed or
	// partial data at the front is dropped a byte at a time so the stream
	// re-syncs by itself after line noise.
	function deframe(buffer) {
		const packets = [];
		let at = 0;

		for (;;) {
			// find the start of frame
			while (at + 1 < buffer.length && !(buffer[at] === 0xab && buffer[at + 1] === 0xcd))
				at++;
			if (at + 8 > buffer.length)
				break;   // need at least an empty packet's worth of bytes

			const len   = buffer[at + 2] | (buffer[at + 3] << 8);
			const total = len + 8;
			if (len > 0x400) { at++; continue; }              // implausible: re-sync
			if (at + total > buffer.length) break;            // wait for the rest

			if (buffer[at + total - 2] !== 0xdc || buffer[at + total - 1] !== 0xba) {
				at++;                                         // bad footer: re-sync
				continue;
			}

			const body    = xorCopy(buffer.subarray(at + 4, at + total - 2));
			const payload = body.subarray(0, len);
			const want    = body[len] | (body[len + 1] << 8);
			// The radio sends 0xFFFF instead of computing a checksum, so treat
			// that as "not supplied" and accept the packet. Any other mismatch
			// is still real corruption and is reported as such.
			packets.push({ payload, crcOk: want === NO_CRC || crc16(payload) === want });
			at += total;
		}

		return { packets, rest: buffer.subarray(at) };
	}

	// ---- commands ----------------------------------------------------------

	// The session token. Any four bytes work as long as they stay the same for
	// the whole session; these are the ones k5prog has always used, so they are
	// known to be accepted by every firmware version out there.
	const SESSION = Uint8Array.of(0x6a, 0x39, 0x57, 0x64);

	const cmd = {
		// normal mode
		hello() {
			return Uint8Array.of(0x14, 0x05, 0x04, 0x00, ...SESSION);
		},
		readEeprom(address, length) {
			return Uint8Array.of(0x1b, 0x05, 0x08, 0x00,
				address & 0xff, (address >> 8) & 0xff, length & 0xff, 0x00, ...SESSION);
		},
		writeEeprom(address, data) {
			const out = new Uint8Array(12 + data.length);
			out.set([0x1d, 0x05, data.length + 8, 0x00,
			         address & 0xff, (address >> 8) & 0xff, data.length & 0xff, 0x01,
			         ...SESSION], 0);
			out.set(data, 12);
			return out;
		},
		reset() {
			return Uint8Array.of(0xdd, 0x05, 0x00, 0x00);
		},

		// bootloader mode
		//
		// The version string is presented to the bootloader before flashing.
		// Anything starting with '*' is accepted by every known bootloader,
		// which is what our own images carry.
		version(versionBytes) {
			const v = new Uint8Array(16);
			v.set(versionBytes.subarray(0, 16));
			return Uint8Array.of(0x30, 0x05, 0x10, 0x00, ...v);
		},

		// One 0x100-byte block of flash. `totalSize` is the whole image length;
		// the bootloader is told the final block address so it knows when the
		// image ends.
		//
		//   0x19 05 0c 01 | 8a 8d 9f 1d | addr_hi addr_lo final_hi 00
		//   | len_hi len_lo 00 00 | 0x100 bytes
		//
		// Note: k5prog writes the length field the other way round and works
		// too, so the bootloader evidently does not check it - the packet
		// length already says how much data there is. This matches the browser
		// flasher, which is the path proven over Web Serial.
		flashBlock(address, data, totalSize) {
			if (data.length > 0x100) throw new Error('flash block too long');
			const finalAddr = (totalSize + 0xff) & ~0xff;
			if (finalAddr > 0xf000) throw new Error('firmware image too large for the flash area');

			const out = new Uint8Array(16 + 0x100);
			out.set([0x19, 0x05, 0x0c, 0x01,
			         0x8a, 0x8d, 0x9f, 0x1d,
			         (address >> 8) & 0xff, address & 0xff, (finalAddr >> 8) & 0xff, 0x00,
			         0x01, 0x00, 0x00, 0x00], 0);
			out.set(data, 16);       // the tail is left as zeroes for a short last block
			return out;
		},
	};

	// ---- firmware images ---------------------------------------------------

	// A ".packed.bin" is the form the official and web flashers accept: the raw
	// image with a 16-byte version string inserted at 0x2000, the whole thing
	// XORed with a 128-byte key, and a CRC-16 appended little-endian. The
	// bootloader itself wants the raw bytes plus the version sent separately.

	const FW_XOR_KEY = Uint8Array.of(
		0x47, 0x22, 0xc0, 0x52, 0x5d, 0x57, 0x48, 0x94, 0xb1, 0x60, 0x60, 0xdb, 0x6f, 0xe3, 0x4c, 0x7c,
		0xd8, 0x4a, 0xd6, 0x8b, 0x30, 0xec, 0x25, 0xe0, 0x4c, 0xd9, 0x00, 0x7f, 0xbf, 0xe3, 0x54, 0x05,
		0xe9, 0x3a, 0x97, 0x6b, 0xb0, 0x6e, 0x0c, 0xfb, 0xb1, 0x1a, 0xe2, 0xc9, 0xc1, 0x56, 0x47, 0xe9,
		0xba, 0xf1, 0x42, 0xb6, 0x67, 0x5f, 0x0f, 0x96, 0xf7, 0xc9, 0x3c, 0x84, 0x1b, 0x26, 0xe1, 0x4e,
		0x3b, 0x6f, 0x66, 0xe6, 0xa0, 0x6a, 0xb0, 0xbf, 0xc6, 0xa5, 0x70, 0x3a, 0xba, 0x18, 0x9e, 0x27,
		0x1a, 0x53, 0x5b, 0x71, 0xb1, 0x94, 0x1e, 0x18, 0xf2, 0xd6, 0x81, 0x02, 0x22, 0xfd, 0x5a, 0x28,
		0x91, 0xdb, 0xba, 0x5d, 0x64, 0xc6, 0xfe, 0x86, 0x83, 0x9c, 0x50, 0x1c, 0x73, 0x03, 0x11, 0xd6,
		0xaf, 0x30, 0xf4, 0x2c, 0x77, 0xb2, 0x7d, 0xbb, 0x3f, 0x29, 0x28, 0x57, 0x22, 0xd6, 0x92, 0x8b);

	const VERSION_OFFSET = 0x2000;
	const VERSION_LENGTH = 16;
	const FLASH_LIMIT    = 0xf000;   // the bootloader owns everything above this

	function fwXor(bytes) {
		const out = Uint8Array.from(bytes);
		for (let i = 0; i < out.length; i++)
			out[i] ^= FW_XOR_KEY[i % FW_XOR_KEY.length];
		return out;
	}

	// Accepts either form and returns { raw, version, packed, crcOk }.
	// `raw` is what goes to the bootloader, `version` the 16 bytes to present.
	function readImage(bytes) {
		const looksPacked = bytes.length > VERSION_OFFSET + VERSION_LENGTH + 2 &&
		                    isPacked(bytes);
		if (!looksPacked) {
			// A raw build: no version inside, so present the wildcard version
			// that every bootloader accepts.
			const version = new Uint8Array(VERSION_LENGTH);
			version.set(new TextEncoder().encode('*'));
			return { raw: Uint8Array.from(bytes), version, packed: false, crcOk: null };
		}

		const payload = bytes.subarray(0, bytes.length - 2);
		const want    = bytes[bytes.length - 2] | (bytes[bytes.length - 1] << 8);
		const crcOk   = crc16(payload) === want;

		const plain   = fwXor(payload);
		const version = plain.slice(VERSION_OFFSET, VERSION_OFFSET + VERSION_LENGTH);
		const raw     = new Uint8Array(plain.length - VERSION_LENGTH);
		raw.set(plain.subarray(0, VERSION_OFFSET), 0);
		raw.set(plain.subarray(VERSION_OFFSET + VERSION_LENGTH), VERSION_OFFSET);
		return { raw, version, packed: true, crcOk };
	}

	// A packed image decodes to a plausible Cortex-M0 vector table: the initial
	// stack pointer sits in SRAM (0x20000000..0x20004000) and the reset vector
	// in the low flash area with the Thumb bit set.
	function isPacked(bytes) {
		const head = fwXor(bytes.subarray(0, 8));
		const sp    = head[0] | (head[1] << 8) | (head[2] << 16) | (head[3] << 24);
		const reset = head[4] | (head[5] << 8) | (head[6] << 16) | (head[7] << 24);
		return sp >>> 0 > 0x20000000 && sp >>> 0 <= 0x20004000 &&
		       (reset & 1) === 1 && (reset >>> 0) < FLASH_LIMIT;
	}

	function versionString(versionBytes) {
		let s = '';
		for (const b of versionBytes) {
			if (b === 0) break;
			s += String.fromCharCode(b);
		}
		return s;
	}

	// A sanity check with a plain-English reason, so the tool can refuse an
	// image before it touches the radio rather than half way through.
	function checkImage(image) {
		if (image.packed && image.crcOk === false)
			return 'This file failed its checksum, so it is damaged or is not a Quansheng firmware image.';
		if (image.raw.length < 0x2000)
			return `This file is only ${image.raw.length} bytes - far too small to be radio firmware.`;
		if (image.raw.length > FLASH_LIMIT)
			return `This image is ${image.raw.length} bytes, larger than the ${FLASH_LIMIT} bytes of program flash in the radio.`;
		const sp    = image.raw[0] | (image.raw[1] << 8) | (image.raw[2] << 16) | (image.raw[3] << 24);
		const reset = image.raw[4] | (image.raw[5] << 8) | (image.raw[6] << 16) | (image.raw[7] << 24);
		if (!(sp >>> 0 > 0x20000000 && sp >>> 0 <= 0x20004000 && (reset & 1) === 1))
			return 'This file does not start like firmware for this radio (bad interrupt vector table).';
		return null;
	}

	return {
		XOR_KEY, SESSION, FLASH_LIMIT, VERSION_OFFSET, VERSION_LENGTH, NO_CRC,
		crc16, frame, deframe, cmd,
		readImage, checkImage, versionString, fwXor, isPacked,
	};
})();

// `const` at the top level of a classic script does not become a window
// property, so publish it explicitly for other scripts, tests and the console.
if (typeof window !== 'undefined') window.K5 = K5;
if (typeof module !== 'undefined' && module.exports) module.exports = K5;
