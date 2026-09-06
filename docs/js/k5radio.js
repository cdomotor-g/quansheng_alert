// Talking to a Quansheng UV-K5 over Web Serial.
//
// One persistent read loop feeds a small queue of decoded packets; everything
// else waits on that queue. The radio may be power-cycled at any time without
// the port going away, because the USB-serial chip lives in the cable, not in
// the radio - which is what lets this tool back up, wait for bootloader mode
// and flash without the user ever unplugging anything.

'use strict';

class K5Radio {
	constructor(log = () => {}) {
		this.port    = null;
		this.log     = log;
		this.queue   = [];          // decoded packets waiting to be claimed
		this.waiters = [];          // pending expect() calls
		this.buffer  = new Uint8Array(0);
		this.reading = false;
		this.onLost  = null;        // called if the cable is pulled
		this.stats   = { bytes: 0, packets: 0, badCrc: 0, beacons: 0 };   // since connect
		this._capture = null;       // raw bytes being collected for listen()
	}

	static get supported() {
		return typeof navigator !== 'undefined' && 'serial' in navigator;
	}

	get connected() {
		return !!(this.port && this.port.readable);
	}

	// ---- connection --------------------------------------------------------

	async connect() {
		if (!K5Radio.supported)
			throw new Error('This browser has no Web Serial support.');

		this.port = await navigator.serial.requestPort();
		await this._open(K5Radio.BAUD);
	}

	async disconnect() {
		await this._close();
		this.port = null;
		this.log('Serial port closed.');
	}

	static get BAUD() { return 38400; }

	// Close and reopen the same port at another rate, without the user having
	// to pick it again. Only the cable check uses this: the radio itself is
	// 38400 and nothing else, so the last thing the check does is come back.
	async reopen(baudRate) {
		if (!this.port) throw new Error('The radio is not connected.');
		await this._close();
		await this._open(baudRate);
	}

	// What the browser knows about the adapter: its USB vendor and product
	// ids, or nothing for a built-in COM port.
	info() {
		const i = (this.port && this.port.getInfo) ? this.port.getInfo() : {};
		return { vid: i.usbVendorId || 0, pid: i.usbProductId || 0 };
	}

	async _open(baudRate) {
		await this.port.open({
			baudRate, dataBits: 8, stopBits: 1, parity: 'none',
			bufferSize: 4096, flowControl: 'none',
		});
		this.baud = baudRate;
		this.log(`Serial port open at ${baudRate} baud.`);
		this._loopDone = this._readLoop();
	}

	async _close() {
		this.reading = false;
		try {
			if (this._reader) { await this._reader.cancel().catch(() => {}); }
		} catch { /* ignore */ }
		// let the read loop release its lock before closing, else close() rejects
		if (this._loopDone) {
			await Promise.race([this._loopDone, new Promise(r => setTimeout(r, 1000))]);
			this._loopDone = null;
		}
		try {
			if (this.port) await this.port.close();
		} catch { /* ignore */ }
		this.queue = [];
		this.buffer = new Uint8Array(0);
		this._rejectAll(new Error('Disconnected.'));
	}

	async _readLoop() {
		this.reading = true;
		while (this.reading && this.port && this.port.readable) {
			this._reader = this.port.readable.getReader();
			try {
				for (;;) {
					const { value, done } = await this._reader.read();
					if (done) break;
					if (value && value.length) this._feed(value);
				}
			} catch (err) {
				if (this.reading) {
					this.log(`Serial read error: ${err.message}`);
					this._rejectAll(err);
					if (this.onLost) this.onLost(err);
				}
				break;
			} finally {
				try { this._reader.releaseLock(); } catch { /* ignore */ }
				this._reader = null;
			}
		}
		this.reading = false;
	}

	_feed(chunk) {
		this.stats.bytes += chunk.length;
		if (this._capture && this._capture.length < 64)
			this._capture.push(...chunk.subarray(0, 64 - this._capture.length));

		const merged = new Uint8Array(this.buffer.length + chunk.length);
		merged.set(this.buffer, 0);
		merged.set(chunk, this.buffer.length);

		const { packets, rest } = K5.deframe(merged);
		this.buffer = rest.length > 4096 ? rest.subarray(rest.length - 4096) : rest;

		for (const pkt of packets) {
			if (!pkt.crcOk) { this.stats.badCrc++; this.log('Ignored a packet with a bad checksum.'); continue; }
			this.stats.packets++;
			if (K5Radio.isBootloaderBroadcast(pkt.payload)) this.stats.beacons++;
			// hand it straight to a waiter if one matches, else queue it
			const idx = this.waiters.findIndex(w => w.want === null || w.want === pkt.payload[0]);
			if (idx >= 0) {
				const w = this.waiters.splice(idx, 1)[0];
				clearTimeout(w.timer);
				w.resolve(pkt.payload);
			} else {
				this.queue.push(pkt.payload);
				if (this.queue.length > 32) this.queue.shift();
			}
		}
	}

	_rejectAll(err) {
		for (const w of this.waiters) { clearTimeout(w.timer); w.reject(err); }
		this.waiters = [];
	}

	// ---- primitives --------------------------------------------------------

	async send(payload) {
		if (!this.connected) throw new Error('The radio is not connected.');
		const writer = this.port.writable.getWriter();
		try { await writer.write(K5.frame(payload)); }
		finally { writer.releaseLock(); }
	}

	// Wait for a packet whose first byte is `want` (or any packet if null).
	expect(want, timeoutMs = 1500) {
		const idx = this.queue.findIndex(p => want === null || p[0] === want);
		if (idx >= 0) return Promise.resolve(this.queue.splice(idx, 1)[0]);

		return new Promise((resolve, reject) => {
			const w = { want, resolve, reject, timer: null };
			w.timer = setTimeout(() => {
				this.waiters = this.waiters.filter(x => x !== w);
				reject(new Error(want === null
					? 'The radio did not answer.'
					: `The radio did not send the expected 0x${want.toString(16)} reply in time.`));
			}, timeoutMs);
			this.waiters.push(w);
		});
	}

	drain() { this.queue = []; }

	// Just watch the line for `ms` and report what arrived: raw byte count,
	// packets that framed and passed their checksum, packets that did not,
	// bootloader beacons among them, and the first bytes seen. Nothing is
	// sent. This is the fault-finding check: a terminal shows the bootloader's
	// beacons as gibberish even when everything is right, because they are
	// scrambled binary, so counting them properly is the only honest test.
	// Send a few plain bytes and see whether they come straight back: the
	// test for an adapter whose TX has been shorted to its RX, with no radio
	// attached. Proves the adapter and its driver before the radio is blamed.
	async loopback(ms = 800) {
		if (!this.connected) throw new Error('The radio is not connected.');
		const probe = new TextEncoder().encode('K5-LOOP-' + Date.now());
		this._capture = [];
		const writer = this.port.writable.getWriter();
		try { await writer.write(probe); } finally { writer.releaseLock(); }
		await new Promise(resolve => setTimeout(resolve, ms));
		const got = Uint8Array.from(this._capture);
		this._capture = null;
		this.drain();
		const ok = got.length === probe.length && got.every((b, i) => b === probe[i]);
		return { ok, sent: probe, got };
	}

	async listen(ms) {
		if (!this.connected) throw new Error('The radio is not connected.');
		const before = { ...this.stats };
		this._capture = [];
		this.drain();
		await new Promise(resolve => setTimeout(resolve, ms));
		const sample = Uint8Array.from(this._capture);
		this._capture = null;
		this.drain();
		const after = this.stats;
		return {
			bytes:   after.bytes   - before.bytes,
			packets: after.packets - before.packets,
			badCrc:  after.badCrc  - before.badCrc,
			beacons: after.beacons - before.beacons,
			sample,
		};
	}

	async request(payload, want, timeoutMs = 1500, attempts = 3) {
		let last;
		for (let i = 0; i < attempts; i++) {
			try {
				// Every command here has exactly one reply, so anything already
				// queued is stale - typically bootloader broadcasts left over
				// from a flash. Dropping them stops an old packet being mistaken
				// for the answer to this command.
				this.drain();
				await this.send(payload);
				return await this.expect(want, timeoutMs);
			} catch (err) {
				last = err;
			}
		}
		throw last;
	}

	// ---- normal mode -------------------------------------------------------

	// Returns the running firmware version, e.g. "2.01.26" or "*ALERTRX 02f24b1".
	//
	// The reply to a hello is 0x15. A radio in the bootloader never sends one -
	// it only broadcasts 0x18 - so rather than let that end in a bare timeout,
	// check for a beacon before giving up and say what is actually wrong.
	async hello(timeoutMs = 1200) {
		let reply;
		try {
			reply = await this.request(K5.cmd.hello(), 0x15, timeoutMs, 2);
		} catch (err) {
			if (this.queue.some(p => K5Radio.isBootloaderBroadcast(p)))
				throw new Error('The radio is in bootloader mode, which cannot report a firmware version.');
			throw err;
		}
		const version = K5.versionString(reply.subarray(4, 20));
		if (!/^[\x20-\x7e]+$/.test(version))
			throw new Error('The radio answered with something that is not a version string.');
		return version;
	}

	// A radio in the bootloader broadcasts 0x18 packets about twice a second and
	// answers nothing else; a radio running normal firmware says nothing unless
	// asked. So: clear the queue, then listen. Anything arriving unprompted is
	// the bootloader. That test does not depend on the exact broadcast contents,
	// which differ between bootloader versions.
	static isBootloaderBroadcast(p) {
		return p[0] === 0x18 && p.length >= 7 &&
		       p[2] === 0x20 && p[3] === 0x00 && p[4] === 0x01 && p[5] === 0x02 && p[6] === 0x02;
	}

	async inBootloader(timeoutMs = 1500) {
		this.drain();
		try {
			await this.expect(0x18, timeoutMs);
			return true;
		} catch { return false; }
	}

	// Poll until the radio appears in bootloader mode. `onTick` is called each
	// second so the page can count down while the user power-cycles the radio.
	async waitForBootloader(timeoutMs = 120000, onTick = () => {}) {
		const deadline = Date.now() + timeoutMs;
		this.drain();
		while (Date.now() < deadline) {
			try {
				await this.expect(0x18, 1000);
				return true;
			} catch { /* keep waiting */ }
			onTick(Math.max(0, Math.round((deadline - Date.now()) / 1000)));
		}
		return false;
	}

	static get EEPROM_SIZE()  { return 0x2000; }
	static get EEPROM_BLOCK() { return 0x80; }

	async readEeprom(onProgress = () => {}) {
		const size  = K5Radio.EEPROM_SIZE, step = K5Radio.EEPROM_BLOCK;
		const out   = new Uint8Array(size);
		for (let addr = 0; addr < size; addr += step) {
			const reply = await this.request(K5.cmd.readEeprom(addr, step), 0x1c, 2000, 3);
			const data  = reply.subarray(8, 8 + step);
			if (data.length !== step)
				throw new Error(`The radio returned ${data.length} bytes for address 0x${addr.toString(16)}, expected ${step}.`);
			out.set(data, addr);
			onProgress((addr + step) / size);
		}
		return out;
	}

	async writeEeprom(bytes, onProgress = () => {}, size = K5Radio.EEPROM_SIZE) {
		const step = K5Radio.EEPROM_BLOCK;
		if (bytes.length < size) throw new Error('That backup file is too short for this radio.');
		for (let addr = 0; addr < size; addr += step) {
			await this.request(K5.cmd.writeEeprom(addr, bytes.subarray(addr, addr + step)), 0x1e, 2000, 3);
			onProgress((addr + step) / size);
		}
	}

	async reset() {
		try { await this.send(K5.cmd.reset()); } catch { /* the radio may already be gone */ }
	}

	// ---- bootloader mode ---------------------------------------------------

	// Write a firmware image. `image` comes from K5.readImage().
	async flash(image, onProgress = () => {}) {
		const raw   = image.raw;
		const total = raw.length;
		if (total > K5.FLASH_LIMIT) throw new Error('Image too large for the radio.');

		this.drain();
		await this.send(K5.cmd.version(image.version));
		await this.expect(0x18, 3000);          // the bootloader acknowledges by carrying on

		for (let addr = 0; addr < total; addr += 0x100) {
			const block = raw.subarray(addr, Math.min(addr + 0x100, total));
			let done = false, lastErr = null;

			for (let attempt = 0; attempt < 3 && !done; attempt++) {
				try {
					await this.send(K5.cmd.flashBlock(addr, block, total));
					// the bootloader keeps broadcasting 0x18 early on; ignore those
					const deadline = Date.now() + 3000;
					for (;;) {
						const reply = await this.expect(null, Math.max(200, deadline - Date.now()));
						if (reply[0] === 0x1a) {
							const echoed = (reply[8] << 8) | reply[9];
							if (echoed !== addr)
								throw new Error(`The radio acknowledged block 0x${echoed.toString(16)} while we sent 0x${addr.toString(16)}.`);
							done = true;
							break;
						}
						if (Date.now() > deadline) throw new Error('No acknowledgement for this block.');
					}
				} catch (err) {
					lastErr = err;
					if (attempt < 2) this.log(`Block 0x${addr.toString(16)} failed (${err.message}) - retrying.`);
				}
			}
			if (!done)
				throw new Error(`Flashing stopped at address 0x${addr.toString(16)}: ${lastErr ? lastErr.message : 'no acknowledgement'}. ` +
				                'The radio is still in bootloader mode - switch it off and on holding PTT, then try again.');

			onProgress(Math.min(1, (addr + 0x100) / total));
		}
	}
}

if (typeof window !== 'undefined') window.K5Radio = K5Radio;
if (typeof module !== 'undefined' && module.exports) module.exports = K5Radio;
