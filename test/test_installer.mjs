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
		mode: 'normal',                       // 'normal' | 'boot'
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

	const send = (payload) => {
		if (controller) controller.enqueue(window.K5.frame(Uint8Array.from(payload)));
	};

	const startBroadcast = () => {
		clearInterval(broadcast);
		broadcast = setInterval(() => {
			if (radio.mode === 'boot')
				send([0x18, 0x05, 0x20, 0x00, 0x01, 0x02, 0x02, ...new Array(0x1d).fill(0)]);
		}, 300);
	};

	window.__setMode = (mode, version) => {
		radio.mode = mode;
		if (version) radio.version = version;
		startBroadcast();
	};

	const handle = (payload) => {
		const cmd = payload[0];

		if (radio.mode === 'boot') {
			if (cmd === 0x30) {                                  // version presented
				radio.versionPresented = new TextDecoder().decode(payload.slice(4)).replace(/\0+$/, '');
				send([0x18, 0x05, 0x20, 0x00, 0x01, 0x02, 0x02, ...new Array(0x1d).fill(0)]);
			} else if (cmd === 0x19) {                           // flash block
				const addr = (payload[8] << 8) | payload[9];
				radio.flashed.set(addr, payload.slice(16, 16 + 0x100));
				send([0x1a, 0x05, 0x08, 0x00, 0x8a, 0x8d, 0x9f, 0x1d, payload[8], payload[9], 0x00, 0x00]);
			}
			return;
		}

		if (cmd === 0x14) {                                      // hello
			const v = new TextEncoder().encode(radio.version);
			const body = new Uint8Array(16);
			body.set(v.subarray(0, 16));
			send([0x18, 0x05, 0x10, 0x00, ...body]);
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

	const port = {
		readable: new ReadableStream({ start(c) { controller = c; } }),
		writable: new WritableStream({
			write(chunk) {
				const merged = new Uint8Array(rxBuffer.length + chunk.length);
				merged.set(rxBuffer, 0);
				merged.set(chunk, rxBuffer.length);
				const { packets, rest } = window.K5.deframe(merged);
				rxBuffer = rest;
				for (const p of packets) if (p.crcOk) handle(p.payload);
			},
		}),
		async open() { radio.opened = true; startBroadcast(); },
		async close() { radio.opened = false; clearInterval(broadcast); },
		getInfo() { return { usbVendorId: 0x1a86, usbProductId: 0x7523 }; },
	};

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
