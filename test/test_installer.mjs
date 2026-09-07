// End-to-end test of the web installer (docs/) against a simulated radio.
//
//   node test/test_installer.mjs
//
// A fake navigator.serial is injected into the page. It speaks the real UV-K5
// protocol - hello, EEPROM reads, bootloader broadcasts, flash blocks - so the
// whole wizard is exercised: pick firmware, connect, back up, wait for
// bootloader mode, flash, verify after reboot. The bytes the "radio" receives
// are compared against the real build, so a framing or ordering mistake fails
// here instead of on someone's hardware.
//
// Needs Playwright and the Chromium at PLAYWRIGHT_BROWSERS_PATH; skips cleanly
// (exit 0) if either is missing, so it never blocks a build machine without them.

import { createServer } from 'node:http';
import { readFile, readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, extname, normalize } from 'node:path';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const docs = join(root, 'docs');

let chromium;
try {
	({ chromium } = await import('playwright'));
} catch {
	console.log('installer: skipped (playwright is not installed)');
	process.exit(0);
}

const CHROME_CANDIDATES = [
	process.env.CHROMIUM_PATH,
	'/opt/pw-browsers/chromium-1194/chrome-linux/chrome',
	'/usr/bin/chromium',
	'/usr/bin/chromium-browser',
	'/usr/bin/google-chrome',
].filter(Boolean);
const executablePath = CHROME_CANDIDATES.find(p => existsSync(p));

if (!existsSync(join(docs, 'firmware/manifest.json'))) {
	console.log('installer: skipped (docs/firmware not built yet)');
	process.exit(0);
}

// ---- a tiny static server for docs/ ----------------------------------------

const TYPES = {
	'.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
	'.json': 'application/json', '.bin': 'application/octet-stream',
};

const server = createServer((req, res) => {
	const rel = decodeURIComponent(req.url.split('?')[0]);
	const path = join(docs, normalize(rel === '/' ? '/index.html' : rel).replace(/^(\.\.[/\\])+/, ''));
	readFile(path, (err, data) => {
		if (err) { res.writeHead(404).end('not found'); return; }
		res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' });
		res.end(data);
	});
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}/`;

// ---- the simulated radio, injected into the page ---------------------------

// Runs in the browser. Builds a fake SerialPort whose writable side decodes
// commands with the page's own K5 codec and whose readable side answers them.
function installFakeRadio() {
	const radio = {
		mode: 'normal',                       // 'normal' | 'boot' | 'noise' | 'loop'
		opens: [],                            // baud rate of every open()
		version: '2.01.26',
		eeprom: null,
		flashed: new Map(),                   // address -> Uint8Array(0x100)
		versionPresented: null,
		opened: false,
	};
	window.__radio = radio;

	// deterministic "calibration" so the backup can be checked byte for byte
	radio.eeprom = new Uint8Array(0x2000);
	for (let i = 0; i < radio.eeprom.length; i++) radio.eeprom[i] = (i * 7 + 3) & 0xff;

	let controller = null;
	let broadcast = null;

	// A real bootloader beacon, captured verbatim off a radio whose bootloader
	// reports 7.00.07. Its device bytes are 52 34 50, not the 01 02 02 of a
	// stock 2.00.06 - which is exactly what beacon detection used to insist on.
	const REAL_BEACON = [
		0x18, 0x05, 0x20, 0x00, 0x52, 0x34, 0x50, 0x41, 0x14, 0x33, 0x33, 0x33,
		0x56, 0x43, 0xa3, 0x00, 0x78, 0x47, 0x04, 0x56, 0x37, 0x2e, 0x30, 0x30,
		0x2e, 0x30, 0x37, 0x00, 0x00, 0x00, 0xc4, 0x20, 0x00, 0x08, 0x00, 0x00];

	// Put `value` in the CRC field of an already-framed packet, obfuscated the
	// way the radio would send it.
	const forceCrc = (frame, len, value) => {
		const key = window.K5.XOR_KEY;
		frame[4 + len]     = (value & 0xff)        ^ key[len % key.length];
		frame[4 + len + 1] = ((value >> 8) & 0xff) ^ key[(len + 1) % key.length];
		return frame;
	};

	// The radio never sends a checksum that verifies, and the two modes get it
	// wrong differently. Reproduce both exactly, or these tests exercise a radio
	// that does not exist:
	//   'none' - 0xFFFF, what the firmware sends
	//   'boot' - 0x6ed1, captured from the 7.00.07 bootloader
	//   'real' - leave the computed CRC, so that branch stays covered too
	const send = (payload, crc = 'none') => {
		if (!controller) return;
		const bytes = Uint8Array.from(payload);
		const frame = window.K5.frame(bytes);
		if (crc === 'none') forceCrc(frame, bytes.length, 0xffff);
		else if (crc === 'boot') forceCrc(frame, bytes.length, 0x6ed1);
		controller.enqueue(frame);
	};

	const startBroadcast = () => {
		clearInterval(broadcast);
		broadcast = setInterval(() => {
			if (radio.mode === 'boot')
				send(REAL_BEACON, 'boot');
			if (radio.mode === 'noise' && controller)      // a line sampled at the wrong rate
				controller.enqueue(Uint8Array.from({ length: 20 }, () => Math.random() * 256));
		}, 300);
	};

	window.__setMode = (mode, version) => {
		radio.mode = mode;
		if (version) radio.version = version;
		startBroadcast();
	};

	const handle = (payload) => {
		const cmd = payload[0];
		if (radio.mode === 'noise') return;                     // a mis-sampled line hears nothing either


		if (radio.mode === 'boot') {
			if (cmd === 0x30) {                                  // version presented
				radio.versionPresented = new TextDecoder().decode(payload.slice(4)).replace(/\0+$/, '');
				send(REAL_BEACON, 'boot');
			} else if (cmd === 0x19) {                           // flash block
				const addr = (payload[8] << 8) | payload[9];
				radio.flashed.set(addr, payload.slice(16, 16 + 0x100));
				send([0x1a, 0x05, 0x08, 0x00, 0x8a, 0x8d, 0x9f, 0x1d, payload[8], payload[9], 0x00, 0x00], 'real');
			}
			return;
		}

		if (cmd === 0x14) {                                      // hello
			const v = new TextEncoder().encode(radio.version);
			const body = new Uint8Array(16);
			body.set(v.subarray(0, 16));
			send([0x15, 0x05, 0x10, 0x00, ...body]);             // 0x15, not 0x18
		} else if (cmd === 0x1b) {                               // read EEPROM
			const addr = payload[4] | (payload[5] << 8);
			const len  = payload[6];
			send([0x1c, 0x05, len + 8, 0x00, payload[4], payload[5], len, 0x00,
			      ...radio.eeprom.slice(addr, addr + len)]);
		} else if (cmd === 0x1d) {                               // write EEPROM
			const addr = payload[4] | (payload[5] << 8);
			const len  = payload[6];
			radio.eeprom.set(payload.slice(12, 12 + len), addr);
			send([0x1e, 0x05, 0x04, 0x00, payload[4], payload[5], len, 0x00]);
		} else if (cmd === 0xdd) {                               // reset
			radio.resetSeen = true;
		}
	};

	let rxBuffer = new Uint8Array(0);

	// Like the real thing, the streams exist only while the port is open and
	// are made afresh on every open(), so the cable check can close and reopen
	// the port at other baud rates.
	const port = {
		readable: null,
		writable: null,
		async open(opts) {
			radio.opened = true;
			radio.opens.push(opts.baudRate);
			this.readable = new ReadableStream({ start(c) { controller = c; }, cancel() { controller = null; } });
			this.writable = new WritableStream({
				write(chunk) {
					if (radio.mode === 'loop') { if (controller) controller.enqueue(Uint8Array.from(chunk)); return; }
					const merged = new Uint8Array(rxBuffer.length + chunk.length);
					merged.set(rxBuffer, 0);
					merged.set(chunk, rxBuffer.length);
					const { packets, rest } = window.K5.deframe(merged);
					rxBuffer = rest;
					for (const p of packets) if (p.crcOk) handle(p.payload);
				},
			});
			startBroadcast();
		},
		async close() {
			radio.opened = false;
			clearInterval(broadcast);
			// A real browser does not reject when something still holds a lock -
			// it simply never settles, which is what used to freeze the page.
			// Model that exactly, on demand.
			if (radio.hangOnClose) return new Promise(() => {});
			if (this.readable.locked || this.writable.locked) throw new DOMException('port is locked', 'InvalidStateError');
			controller = null;
			this.readable = null;
			this.writable = null;
		},
		getInfo() { return { usbVendorId: 0x1a86, usbProductId: 0x7523 }; },

		// Real SerialPorts are EventTargets and fire 'disconnect' when the
		// device goes away.
		_listeners: {},
		addEventListener(type, fn) { (this._listeners[type] = this._listeners[type] || []).push(fn); },
		removeEventListener(type, fn) {
			this._listeners[type] = (this._listeners[type] || []).filter(f => f !== fn);
		},
	};

	// let the test pull the plug
	window.__unplug = () => { for (const f of port._listeners.disconnect || []) f({ type: 'disconnect' }); };
	window.__hangOnClose = (on) => { radio.hangOnClose = on; };

	Object.defineProperty(navigator, 'serial', {
		configurable: true,
		value: { async requestPort() { return port; }, async getPorts() { return [port]; },
		         addEventListener() {}, removeEventListener() {} },
	});
}

// ---- drive the page --------------------------------------------------------

let failures = 0;
const check = (name, cond, extra = '') => {
	if (cond) { console.log(`  ok    ${name}`); }
	else { failures++; console.log(`  FAIL  ${name}${extra ? '\n        ' + extra : ''}`); }
};

console.log('web installer (simulated radio)');

const browser = await chromium.launch(executablePath ? { executablePath } : {});
const context = await browser.newContext({ acceptDownloads: true });
const page = await context.newPage();

const errors = [];
page.on('pageerror', e => errors.push(e.message));
page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });

await page.addInitScript(installFakeRadio);
await page.goto(base, { waitUntil: 'networkidle' });

try {
	// --- step 1: the page loads and offers the built-in firmware
	check('the browser-support blocker stays hidden', await page.locator('#unsupported').isHidden());
	await page.waitForSelector('.fw', { timeout: 5000 });
	const cards = await page.locator('.fw').count();
	check('both firmware builds are offered', cards === 2, `saw ${cards} cards`);

	await page.locator('.fw').first().click();
	await page.waitForFunction(() => document.getElementById('fw-chosen').textContent.includes('Ready to install'), null, { timeout: 5000 });
	const chosen = await page.locator('#fw-chosen').textContent();
	check('the chosen build is validated and described', /Ready to install.*checksum OK/.test(chosen), chosen);

	// --- step 2: connect, and the radio is identified
	await page.locator('#btn-connect').click();
	await page.waitForFunction(() => document.getElementById('connect-status').textContent.includes('2.01.26'), null, { timeout: 5000 });
	check('the running firmware version is read back', true);

	// --- the port is single-access, so holding it must be visible, releasable
	//     from anywhere on the page, and undoable without a reload.
	check('the sticky port bar shows while the port is held',
	      await page.locator('#portbar').isVisible());

	await page.locator('#btn-release').click();
	await page.waitForFunction(() => document.getElementById('portbar').hidden, null, { timeout: 5000 });
	check('releasing from the sticky bar actually closes the port',
	      await page.evaluate(() => !window.__radio.opened));

	await page.locator('#btn-connect').click();
	await page.waitForFunction(() => document.getElementById('connect-status').textContent.includes('2.01.26'), null, { timeout: 8000 });
	check('the radio reconnects without reloading the page', true);

	// The freeze the user hit: a close() that never settles must not take the
	// page with it.
	await page.evaluate(() => window.__hangOnClose(true));
	const releaseStart = Date.now();
	await page.locator('#btn-release').click();
	await page.waitForFunction(() => document.getElementById('portbar').hidden, null, { timeout: 15000 });
	const releaseMs = Date.now() - releaseStart;
	check('a close() that never returns still frees the UI', releaseMs < 12000, `took ${releaseMs} ms`);
	await page.evaluate(() => window.__hangOnClose(false));

	// Pulling the cable must not strand the UI in "connected".
	await page.locator('#btn-connect').click();
	await page.waitForFunction(() => !document.getElementById('portbar').hidden, null, { timeout: 8000 });
	await page.evaluate(() => window.__unplug());
	await page.waitForFunction(() => document.getElementById('portbar').hidden, null, { timeout: 5000 });
	check('unplugging the cable resets the connection state', true);

	// back to a good connection for everything that follows
	await page.locator('#btn-connect').click();
	await page.waitForFunction(() => document.getElementById('connect-status').textContent.includes('2.01.26'), null, { timeout: 8000 });

	// --- toolbox: checking the cable with a radio that is on normally ends in a hello
	const cableCheck = async (timeout = 15000) => {
		await page.locator('#btn-cable').evaluate(el => { el.closest('details').open = true; });
		await page.evaluate(() => { document.getElementById('cable-status').textContent = ''; });
		await page.locator('#btn-cable').click();
		await page.waitForFunction(() => /answered|arrived|silence|valid|stopped|packets|beacons/i.test(document.getElementById('cable-status').textContent), null, { timeout });
		return page.locator('#cable-status').textContent();
	};
	const quiet = await cableCheck();
	check('cable check on a radio on normally reports its firmware via hello', /switched on normally.*2\.01\.26/.test(quiet), quiet);
	const report = await page.locator('#cable-report').textContent();
	check('the cable report names the adapter chip', /CH340/.test(report), report);

	// --- step 3: backup
	const downloadPromise = page.waitForEvent('download', { timeout: 30000 });
	await page.locator('#btn-backup').click();
	const download = await downloadPromise;
	await page.waitForFunction(() => document.getElementById('backup-status').textContent.includes('Saved as'), null, { timeout: 30000 });

	const savedPath = await download.path();
	const saved = new Uint8Array(readFileSync(savedPath));
	const expected = await page.evaluate(() => Array.from(window.__radio.eeprom));
	check('the backup file is the radio\'s full 8 KB EEPROM', saved.length === 0x2000, `got ${saved.length} bytes`);
	check('every backed-up byte matches the radio', saved.every((b, i) => b === expected[i]));
	check('the backup file is named for the date', /^uvk5-backup-[\d-]+\.bin$/.test(download.suggestedFilename()),
	      download.suggestedFilename());

	// --- step 4: flashing waits for bootloader mode, then writes the image
	await page.locator('#btn-flash').click();
	await page.waitForFunction(() => document.getElementById('flash-status').textContent.includes('Waiting for the radio'), null, { timeout: 5000 });
	check('the installer waits for the radio to be put in bootloader mode', true);

	await page.evaluate(() => window.__setMode('boot'));
	await page.waitForFunction(() => document.getElementById('flash-status').textContent.includes('Installed'), null, { timeout: 60000 });

	const result = await page.evaluate(() => ({
		version: window.__radio.versionPresented,
		blocks: Array.from(window.__radio.flashed.keys()).sort((a, b) => a - b),
		bytes: Object.fromEntries(Array.from(window.__radio.flashed, ([k, v]) => [k, Array.from(v)])),
	}));

	const raw = new Uint8Array(readFileSync(join(docs, 'firmware/quansheng-alert-default.bin')));
	const expectedBlocks = Math.ceil(raw.length / 0x100);
	check('every block of the image was written', result.blocks.length === expectedBlocks,
	      `wrote ${result.blocks.length} blocks, expected ${expectedBlocks}`);
	check('blocks were written in order from address 0',
	      result.blocks.every((addr, i) => addr === i * 0x100));
	check('the version string was presented to the bootloader first',
	      typeof result.version === 'string' && result.version.startsWith('*'), String(result.version));

	let mismatch = null;
	for (let addr = 0; addr < raw.length && !mismatch; addr += 0x100) {
		const got = result.bytes[addr];
		for (let i = 0; i < 0x100; i++) {
			const want = addr + i < raw.length ? raw[addr + i] : 0;   // short last block is zero-padded
			if (got[i] !== want) { mismatch = `at 0x${(addr + i).toString(16)}: got ${got[i]}, want ${want}`; break; }
		}
	}
	check('the radio received the firmware byte for byte', !mismatch, mismatch || '');

	// --- step 5: verification after the radio reboots
	await page.evaluate(() => window.__setMode('normal', '*ALERTRX test'));
	await page.waitForFunction(() => document.getElementById('done-summary').textContent.includes('Confirmed'), null, { timeout: 60000 });
	const summary = await page.locator('#done-summary').textContent();
	check('the new firmware is confirmed after the reboot', summary.includes('*ALERTRX test'), summary);

	// --- the safety net: the backup is retrievable from the browser afterwards
	const stored = await page.evaluate(() => JSON.parse(localStorage.getItem('quansheng_alert.history') || '[]').map(i => i.kind));
	check('the backup and the install are both recorded on this computer',
	      stored.includes('backup') && stored.includes('install'), JSON.stringify(stored));

	// --- toolbox: the cable check with a radio in bootloader mode counts its beacons
	await page.evaluate(() => window.__setMode('boot'));
	const booted = await cableCheck();
	const count = +(booted.match(/(\d+) beacons/) || [])[1];
	check('cable check on a radio in bootloader mode counts its beacons', /bootloader mode and the cable is good/.test(booted) && count >= 6, booted);

	// --- toolbox: bytes that never frame trigger the baud scan, which comes back to 38400
	await page.evaluate(() => { window.__radio.opens = []; window.__setMode('noise'); });
	const noisy = await cableCheck(60000);
	check('unframeable bytes trigger a baud scan whose verdict is not the rate', /no valid packet at any of them/.test(noisy), noisy);
	const opens = await page.evaluate(() => window.__radio.opens);
	check('the scan tried every rate and left the port back at 38400',
	      opens.length >= 10 && opens.includes(115200) && opens.includes(9600) && opens[opens.length - 1] === 38400, JSON.stringify(opens));

	// --- toolbox: the adapter-only loopback
	await page.evaluate(() => window.__setMode('loop'));
	await page.locator('#btn-loopback').click();
	await page.waitForFunction(() => /echoed|garbled|No echo|stopped/.test(document.getElementById('cable-status').textContent), null, { timeout: 10000 });
	const loop = await page.locator('#cable-status').textContent();
	check('a shorted adapter echoes the loopback probe', /echoed the test string/.test(loop), loop);
	await page.evaluate(() => window.__setMode('normal'));

	check('no JavaScript errors on the page', errors.length === 0, errors.join('\n        '));
} catch (err) {
	failures++;
	console.log(`  FAIL  unexpected error\n        ${err.message}`);
	if (errors.length) console.log(`        page errors: ${errors.join(' | ')}`);
} finally {
	await browser.close();
	server.close();
}

console.log(failures ? `\nFAILED: ${failures}` : '\nPASSED');
process.exit(failures ? 1 : 0);
