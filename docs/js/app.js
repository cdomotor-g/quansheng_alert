// The installer wizard.
'use strict';

const $  = (id) => document.getElementById(id);
const on = (id, ev, fn) => $(id).addEventListener(ev, fn);

const state = {
	radio: null,
	image: null,          // { raw, version, packed, crcOk }
	imageName: '',
	imageMeta: null,      // manifest entry, when it came from this site
	backupDone: false,
	flashed: false,
};

// ---------------------------------------------------------------- logging ---

const logLines = [];
function log(msg) {
	const line = `${new Date().toLocaleTimeString()}  ${msg}`;
	logLines.push(line);
	const el = $('log');
	el.textContent = logLines.join('\n');
	el.scrollTop = el.scrollHeight;
	console.log(line);
}

function say(id, msg, kind = '') {
	const el = $(id);
	el.textContent = msg;
	el.className = 'status' + (kind ? ' ' + kind : '');
}

function progress(id, fraction) {
	const box = $(id);
	box.hidden = false;
	box.firstElementChild.style.width = `${Math.round(fraction * 100)}%`;
}

// ------------------------------------------------------------------ steps ---

const STEPS = ['step-fw', 'step-connect', 'step-backup', 'step-flash', 'step-done'];
let current = 0;

function setStep(index) {
	current = index;
	STEPS.forEach((id, i) => {
		const el = $(id);
		el.classList.toggle('locked', i > index);
		el.classList.toggle('current', i === index);
		el.classList.toggle('done', i < index);
	});
	if (index > 0) $(STEPS[index]).scrollIntoView({ behavior: 'smooth', block: 'center' });
}

function unlock(index) {          // reach a step without jumping backwards
	if (index > current) setStep(index);
}

// ------------------------------------------------------------ file helpers --

function download(bytes, filename) {
	const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
	const a = document.createElement('a');
	a.href = url;
	a.download = filename;
	document.body.appendChild(a);
	a.click();
	a.remove();
	setTimeout(() => URL.revokeObjectURL(url), 10000);
}

const stamp = () => new Date().toISOString().slice(0, 16).replace(/[:T]/g, '-');

function readFile(file) {
	return new Promise((resolve, reject) => {
		const r = new FileReader();
		r.onload  = () => resolve(new Uint8Array(r.result));
		r.onerror = () => reject(r.error);
		r.readAsArrayBuffer(file);
	});
}

// --------------------------------------------------------- browser support --

function checkSupport() {
	if (K5Radio.supported) return true;

	$('wizard').hidden   = true;
	$('toolbox').hidden  = true;
	$('unsupported').hidden = false;

	const ua = navigator.userAgent;
	let why = '';
	if (/iPhone|iPad|iPod/.test(ua))        why = 'This looks like an iPhone or iPad. iOS has no Web Serial at all, in any browser.';
	else if (/Android/.test(ua))            why = 'This looks like an Android device. Chrome on Android has no Web Serial.';
	else if (/Firefox\//.test(ua))          why = 'This looks like Firefox, which has chosen not to implement Web Serial.';
	else if (/Safari\//.test(ua) && !/Chrome|Chromium|Edg/.test(ua))
	                                        why = 'This looks like Safari, which has no Web Serial.';
	else                                    why = 'Your browser did not offer the Web Serial interface this page needs.';
	$('unsupported-why').textContent = why;
	return false;
}

// ------------------------------------------------------- step 1: firmware ---

async function loadManifest() {
	try {
		const res = await fetch('firmware/manifest.json', { cache: 'no-cache' });
		if (!res.ok) throw new Error(`HTTP ${res.status}`);
		return await res.json();
	} catch (err) {
		log(`Could not load the firmware list: ${err.message}`);
		return null;
	}
}

function renderFirmwareCards(manifest) {
	const box = $('fw-cards');
	box.innerHTML = '';

	if (!manifest || !manifest.builds || !manifest.builds.length) {
		box.innerHTML = '<p class="warn">The built-in firmware list could not be loaded. ' +
			'You can still install a file of your own using the option below.</p>';
		return;
	}

	$('fw-loading')?.remove();
	for (const b of manifest.builds) {
		const btn = document.createElement('button');
		btn.type = 'button';
		btn.className = 'fw';
		btn.setAttribute('aria-pressed', 'false');
		btn.innerHTML =
			`<span class="name">${escapeHtml(b.title)}</span>` +
			`<span class="meta">${escapeHtml(b.description)}</span>` +
			`<span class="meta">${b.stations ? b.stations + ' stations · ' : ''}` +
			`${(b.size / 1024).toFixed(1)} KB · built ${escapeHtml(b.built || manifest.built || 'unknown')}</span>`;
		btn.addEventListener('click', () => pickBuild(b, btn));
		box.appendChild(btn);
	}
}

function escapeHtml(s) {
	return String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

async function pickBuild(build, btn) {
	document.querySelectorAll('.fw').forEach(b => b.setAttribute('aria-pressed', 'false'));
	btn.setAttribute('aria-pressed', 'true');
	say('fw-chosen', '');
	try {
		log(`Fetching ${build.file}…`);
		const res = await fetch(build.file, { cache: 'no-cache' });
		if (!res.ok) throw new Error(`HTTP ${res.status}`);
		const bytes = new Uint8Array(await res.arrayBuffer());
		acceptImage(bytes, build.file.split('/').pop(), build);
	} catch (err) {
		$('fw-chosen').hidden = false;
		$('fw-chosen').textContent = `That build could not be downloaded (${err.message}). Try again, or use your own file.`;
		log(`Firmware fetch failed: ${err.message}`);
	}
}

function acceptImage(bytes, name, meta = null) {
	const image  = K5.readImage(bytes);
	const problem = K5.checkImage(image);
	const box = $('fw-chosen');
	box.hidden = false;

	if (problem) {
		box.style.borderLeftColor = 'var(--bad)';
		box.textContent = problem;
		state.image = null;
		log(`Rejected ${name}: ${problem}`);
		return;
	}

	state.image     = image;
	state.imageName = name;
	state.imageMeta = meta;
	box.style.borderLeftColor = '';
	const version = K5.versionString(image.version) || '(no version string)';
	box.textContent = `Ready to install: ${meta ? meta.title : name} — ${(image.raw.length / 1024).toFixed(1)} KB, ` +
		`version ${version}${image.packed ? ', checksum OK' : ''}.`;
	log(`Selected ${name}: ${image.raw.length} bytes, version "${version}", ` +
	    `${image.packed ? 'packed' : 'raw'}${image.crcOk === false ? ', CRC BAD' : ''}.`);
	unlock(1);
}

// -------------------------------------------------------- step 2: connect ---

async function connect() {
	const radio = new K5Radio(log);
	try {
		say('connect-status', 'Choose the cable in the browser pop-up…', 'busy');
		await radio.connect();
	} catch (err) {
		if (err.name === 'NotFoundError') {
			say('connect-status', 'No port was chosen. Press Connect again and pick the cable from the list.', 'bad');
		} else {
			say('connect-status', `Could not open the port: ${err.message}`, 'bad');
		}
		log(`Connect failed: ${err.message}`);
		return;
	}

	state.radio = radio;
	radio.onLost = () => {
		say('connect-status', 'The cable was unplugged.', 'bad');
		$('btn-connect').hidden = false;
		$('btn-disconnect').hidden = true;
		state.radio = null;
	};
	$('btn-connect').hidden = true;
	$('btn-disconnect').hidden = false;

	// Work out what state the radio is in, so the wizard can say something useful.
	say('connect-status', 'Connected. Asking the radio what it is…', 'busy');
	if (await radio.inBootloader(1200)) {
		say('connect-status',
			'Connected — the radio is already in bootloader mode. That means it cannot be backed up ' +
			'(the backup needs the radio switched on normally). Either switch it off and on normally and ' +
			'press "Back up my radio", or tick "skip this" to go straight to installing.', 'ok');
		log('Radio detected in bootloader mode.');
		unlock(2);
		return;
	}

	try {
		const version = await radio.hello(1500);
		say('connect-status', `Connected. This radio is running: ${version}`, 'ok');
		log(`Radio firmware: ${version}`);
	} catch (err) {
		say('connect-status',
			'The port opened but the radio did not answer. Check both plugs are pushed all the way in and ' +
			'that the radio is switched on, then press Connect again.', 'bad');
		log(`Hello failed: ${err.message}`);
	}
	unlock(2);
}

async function disconnect() {
	if (state.radio) await state.radio.disconnect();
	state.radio = null;
	$('btn-connect').hidden = false;
	$('btn-disconnect').hidden = true;
	say('connect-status', 'Disconnected.');
}

// --------------------------------------------------------- step 3: backup ---

async function backup() {
	if (!state.radio) { say('backup-status', 'Connect to the radio first (step 2).', 'bad'); return; }
	$('btn-backup').disabled = true;
	say('backup-status', 'Reading the radio… this takes about ten seconds.', 'busy');
	try {
		const bytes = await state.radio.readEeprom(f => progress('backup-progress', f));
		const name  = `uvk5-backup-${stamp()}.bin`;
		download(bytes, name);
		remember(name, bytes);
		state.backupDone = true;
		say('backup-status', `Saved as ${name} in your Downloads folder. A copy is kept in this browser too, ` +
			'under "Backups and installs on this computer".', 'ok');
		log(`EEPROM backup complete: ${bytes.length} bytes -> ${name}`);
		unlock(3);
	} catch (err) {
		say('backup-status',
			`The backup did not finish: ${err.message} — the radio must be switched on normally ` +
			'(not in bootloader mode) for this step.', 'bad');
		log(`Backup failed: ${err.message}`);
	} finally {
		$('btn-backup').disabled = false;
	}
}

// ---------------------------------------------------------- step 4: flash ---

async function flash() {
	if (!state.image) { say('flash-status', 'Pick a firmware in step 1 first.', 'bad'); return; }
	if (!state.radio) { say('flash-status', 'Connect to the radio first (step 2).', 'bad'); return; }

	$('btn-flash').disabled = true;
	try {
		if (!await state.radio.inBootloader(800)) {
			say('flash-status',
				'Waiting for the radio in bootloader mode — switch it off, hold PTT, switch it on. ' +
				'The white torch LED comes on.', 'busy');
			const found = await state.radio.waitForBootloader(120000, secs => {
				say('flash-status', `Waiting for the radio in bootloader mode… (${secs}s left). ` +
					'Switch it off, hold PTT, switch it on — the white torch LED comes on.', 'busy');
			});
			if (!found) {
				say('flash-status',
					'The radio never appeared in bootloader mode. Switch it off, hold the PTT button down, ' +
					'and switch it on while still holding PTT — the torch LED should glow white and the screen ' +
					'stay blank. Then press Install again.', 'bad');
				return;
			}
		}

		log(`Flashing ${state.imageName} (${state.image.raw.length} bytes)…`);
		say('flash-status', 'Installing — do not unplug anything.', 'busy');
		await state.radio.flash(state.image, f => progress('flash-progress', f));

		state.flashed = true;
		rememberInstall();
		say('flash-status', 'Installed. Now switch the radio off and on again normally.', 'ok');
		log('Flash complete.');
		unlock(4);
		verifyAfterReboot();
	} catch (err) {
		say('flash-status', err.message, 'bad');
		log(`Flash failed: ${err.message}`);
	} finally {
		$('btn-flash').disabled = false;
	}
}

// After a flash, watch for the radio coming back on normal firmware and read
// its version, so the user gets told it actually worked rather than hoping.
async function verifyAfterReboot() {
	$('done-summary').textContent =
		'Switch the radio off and on again (normally, without PTT) and this page will confirm what it is running…';
	const deadline = Date.now() + 90000;
	while (Date.now() < deadline && state.radio && state.radio.connected) {
		try {
			const version = await state.radio.hello(1200);
			$('done-summary').textContent = `Confirmed: the radio is now running ${version}.`;
			$('done-summary').className = 'status ok';
			log(`Verified after reboot: ${version}`);
			return;
		} catch { /* not back yet */ }
	}
	$('done-summary').textContent =
		'Installed. (This page could not read the version back automatically — that is only the check step, ' +
		'not the install. Switch the radio on normally and look at the boot screen.)';
}

// ------------------------------------------------------------- local store --

const STORE = 'quansheng_alert.history';

function history() {
	try { return JSON.parse(localStorage.getItem(STORE) || '[]'); }
	catch { return []; }
}

function saveHistory(items) {
	try { localStorage.setItem(STORE, JSON.stringify(items.slice(0, 8))); }
	catch (err) { log(`Could not save to browser storage: ${err.message}`); }
	renderHistory();
}

function remember(name, bytes) {
	const items = history();
	items.unshift({
		kind: 'backup', name, when: new Date().toISOString(),
		size: bytes.length, data: bytesToBase64(bytes),
	});
	// keep at most three EEPROM copies; they are 8 KB each
	let backups = 0;
	saveHistory(items.filter(i => i.kind !== 'backup' || ++backups <= 3));
}

function rememberInstall() {
	const items = history();
	items.unshift({
		kind: 'install',
		name: state.imageMeta ? state.imageMeta.title : state.imageName,
		when: new Date().toISOString(),
		size: state.image.raw.length,
		version: K5.versionString(state.image.version),
	});
	saveHistory(items);
}

function bytesToBase64(bytes) {
	let s = '';
	for (const b of bytes) s += String.fromCharCode(b);
	return btoa(s);
}

function base64ToBytes(b64) {
	const s = atob(b64);
	const out = new Uint8Array(s.length);
	for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
	return out;
}

function renderHistory() {
	const items = history();
	const box = $('history');
	if (!items.length) { box.innerHTML = '<p class="muted">Nothing recorded yet.</p>'; return; }

	const rows = items.map((it, i) => {
		const when = new Date(it.when).toLocaleString();
		const what = it.kind === 'backup'
			? `Backup of your radio (${(it.size / 1024).toFixed(0)} KB)`
			: `Installed ${escapeHtml(it.name)}${it.version ? ' — ' + escapeHtml(it.version) : ''}`;
		const action = it.kind === 'backup'
			? `<button class="ghost" data-save="${i}">Save again</button>`
			: '';
		return `<tr><td>${when}</td><td>${what}</td><td>${action}</td></tr>`;
	}).join('');

	box.innerHTML = `<table class="hist"><thead><tr><th>When</th><th>What</th><th></th></tr></thead><tbody>${rows}</tbody></table>`;
	box.querySelectorAll('[data-save]').forEach(btn => {
		btn.addEventListener('click', () => {
			const it = history()[+btn.dataset.save];
			download(base64ToBytes(it.data), it.name);
		});
	});
}

// ---------------------------------------------------------------- toolbox ---

async function identify() {
	if (!state.radio) { say('identify-status', 'Connect to the radio first (step 2).', 'bad'); return; }
	try {
		if (await state.radio.inBootloader(800)) {
			say('identify-status', 'The radio is in bootloader mode, which cannot report a firmware version. ' +
				'Switch it off and on normally and ask again.', 'bad');
			return;
		}
		const version = await state.radio.hello(1500);
		say('identify-status', `This radio is running: ${version}`, 'ok');
	} catch (err) {
		say('identify-status', `The radio did not answer: ${err.message}`, 'bad');
	}
}

// Toolbox -> Check the cable. The browser-side version of tools/k5diag.py:
// identify the adapter, listen at 38400, ask a hello, and only if bytes are
// arriving that do not frame, try the other rates. Each step is written to a
// report the user can copy, and the verdict names a side to look at.

const CHIPS = {
	'1a86:7523': 'CH340, the usual K5 cable chip',
	'1a86:55d4': 'CH9102',
	'10c4:ea60': 'CP210x',
	'067b:2303': 'PL2303 - often counterfeit, and current Windows drivers refuse those',
	'067b:23a3': 'PL2303GC',
	'0403:6001': 'FTDI FT232',
};
const SCAN_BAUDS = [38400, 115200, 57600, 19200, 9600, 76800, 128000, 230400, 4800, 460800];

const hexOf = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join(' ');

function reportTo(id) {
	const box = $(id);
	box.textContent = '';
	box.hidden = false;
	return (line) => {
		box.textContent += (box.textContent ? '\n' : '') + line;
		log(line);
	};
}

async function checkCable() {
	if (!state.radio) { say('cable-status', 'Connect to the radio first (step 2).', 'bad'); return; }
	const radio = state.radio;
	const line  = reportTo('cable-report');
	const done  = (msg, kind) => { say('cable-status', msg, kind); line('=> ' + msg); };
	$('btn-cable').disabled = true;
	$('btn-loopback').disabled = true;
	try {
		// 1. the adapter
		const { vid, pid } = radio.info();
		if (vid) {
			const key = `${vid.toString(16).padStart(4, '0')}:${pid.toString(16).padStart(4, '0')}`;
			line(`Adapter: USB ${key}` + (CHIPS[key] ? ` - ${CHIPS[key]}` : ''));
		} else {
			line('Adapter: no USB identity (a built-in COM port, or a driver that hides it)');
		}

		// 2. listen at the radio's one and only rate
		const secs = 3;
		say('cable-status', `Listening at 38400 baud for ${secs} seconds…`, 'busy');
		if (radio.baud !== K5Radio.BAUD) await radio.reopen(K5Radio.BAUD);
		const r = await radio.listen(secs * 1000);
		line(`Listened ${secs} s at 38400: ${r.bytes} bytes, ${r.packets} good packets, ${r.badCrc} bad checksums, ` +
		     `${r.beacons} bootloader beacons` + (r.sample.length ? `\n  first bytes: ${hexOf(r.sample)}` : ''));

		if (r.beacons) {
			done(`The radio is in bootloader mode and the cable is good: ${r.beacons} beacons in ${secs} s with ` +
			     `correct checksums at 38400 baud${r.badCrc ? ` (${r.badCrc} damaged, so the line is a little noisy)` : ''}. ` +
			     'That proves the wiring, the polarity and the baud rate. If installing still fails, the cause is on ' +
			     'this computer: usually another program (a terminal, CHIRP, another tab) holding the port, or the ' +
			     'browser being given the wrong port.', 'ok');
			return;
		}
		if (r.packets) {
			done(`${r.packets} packets arrived and passed their checksum, but none were bootloader beacons. ` +
			     'The line is good. Use "What is my radio running?" to identify the firmware.', 'ok');
			return;
		}

		// 3. a radio switched on normally says nothing until asked
		say('cable-status', 'Nothing unprompted. Asking the radio for its version…', 'busy');
		try {
			const version = await radio.hello(1500);
			line(`Hello: answered, running ${version}`);
			done(`Nothing was sent unprompted, but the radio answered when asked: it is switched on normally ` +
			     `and running ${version}. The cable works in both directions and the baud rate is right.`, 'ok');
			return;
		} catch {
			line('Hello: no answer');
		}

		// 4. bytes that do not frame: is it really the rate? Try the others.
		if (!r.bytes) {
			done(`Nothing arrived in ${secs} s and the radio did not answer a hello. If it is in bootloader ` +
			     'mode (white torch LED, blank screen), its transmit line is not reaching the adapter: swap TX ' +
			     'and RX at the adapter end, and check the 2.5 mm plug is fully home. If it is switched on ' +
			     'normally, check the same things plus the 3.5 mm plug. To rule the adapter itself out, short ' +
			     'its TX to its RX and press "Test the adapter alone".', 'bad');
			return;
		}

		line('');
		line('Bytes arrived that did not frame, so trying every plausible rate (radio in bootloader mode, please):');
		line('   baud   bytes  packets  beacons');
		const results = {};
		for (const baud of SCAN_BAUDS) {
			say('cable-status', `Trying ${baud} baud…`, 'busy');
			try {
				await radio.reopen(baud);
			} catch (err) {
				// some drivers refuse the odd rates; that says nothing about the radio
				line(`${String(baud).padStart(7)}  the driver would not open the port at this rate`);
				continue;
			}
			const s = await radio.listen(1500);
			results[baud] = s;
			line(`${String(baud).padStart(7)}  ${String(s.bytes).padStart(6)}  ${String(s.packets).padStart(7)}  ${String(s.beacons).padStart(7)}`);
		}
		await radio.reopen(K5Radio.BAUD);

		const framed = SCAN_BAUDS.filter(b => results[b] && results[b].packets);
		const noisy  = SCAN_BAUDS.filter(b => results[b] && results[b].bytes);
		if (framed.includes(K5Radio.BAUD)) {
			done('Valid packets at 38400 this time - the standard rate - so the earlier run caught noise. ' +
			     'Nothing needs changing; run the check again.', 'ok');
		} else if (framed.length) {
			done(`Valid packets only at ${framed[0]} baud, which no known UV-K5 bootloader uses. Check the ` +
			     'adapter\'s driver is not applying a divisor, and that this is really a UV-K5 (the V3/UV-K1 is a different chip).', 'bad');
		} else if (noisy.length) {
			done('Bytes at some rates but no valid packet at any of them. A baud mismatch would have shown ' +
			     'up in the table, so it is not the rate: look for an inverted signal (some adapters and cables ' +
			     'invert), plugs not fully home, or a 5 V adapter loading the line.', 'bad');
		} else {
			done('Silence at every rate. The radio\'s transmit line is not reaching the adapter: wrong contact ' +
			     'on the 2.5 mm plug, TX/RX swapped, or the radio is not actually in bootloader mode.', 'bad');
		}
	} catch (err) {
		say('cable-status', `The check stopped: ${err.message}`, 'bad');
		line(`Stopped: ${err.message}`);
	} finally {
		if (radio.connected && radio.baud !== K5Radio.BAUD) {
			try { await radio.reopen(K5Radio.BAUD); } catch { /* reported above */ }
		}
		$('btn-cable').disabled = false;
		$('btn-loopback').disabled = false;
	}
}

// Toolbox -> Test the adapter alone. Adapter TX shorted to RX, no radio.
async function testAdapter() {
	if (!state.radio) { say('cable-status', 'Connect to the adapter first (step 2).', 'bad'); return; }
	const line = reportTo('cable-report');
	$('btn-cable').disabled = true;
	$('btn-loopback').disabled = true;
	say('cable-status', 'Sending a test string and waiting for it to come back…', 'busy');
	try {
		const r = await state.radio.loopback();
		line(`Loopback: sent ${hexOf(r.sent)}` + (r.got.length ? `\n  got  ${hexOf(r.got)}` : '\n  got nothing'));
		if (r.ok) {
			say('cable-status', 'The adapter echoed the test string: the adapter and its driver work at 38400 baud. ' +
			    'Now remove the short, plug the radio in and run "Check the cable".', 'ok');
		} else if (r.got.length) {
			say('cable-status', 'A garbled echo came back: the adapter is talking to itself but corrupting data. ' +
			    'Try another USB port or another adapter; counterfeit PL2303s do this.', 'bad');
		} else {
			say('cable-status', 'No echo. Either TX and RX are not actually shorted, or the adapter or its driver ' +
			    'is dead. Nothing about the radio can be tested until this passes.', 'bad');
		}
	} catch (err) {
		say('cable-status', `The test stopped: ${err.message}`, 'bad');
	} finally {
		$('btn-cable').disabled = false;
		$('btn-loopback').disabled = false;
	}
}

let restoreBytes = null;

async function pickRestoreFile(ev) {
	const file = ev.target.files[0];
	if (!file) return;
	const bytes = await readFile(file);
	if (bytes.length < K5Radio.EEPROM_SIZE) {
		say('restore-status', `That file is ${bytes.length} bytes; a backup of this radio is ${K5Radio.EEPROM_SIZE} bytes.`, 'bad');
		$('btn-restore').disabled = true;
		restoreBytes = null;
		return;
	}
	restoreBytes = bytes;
	$('btn-restore').disabled = false;
	say('restore-status', `${file.name} looks like a valid backup. Press the button to write it back.`);
}

async function restore() {
	if (!restoreBytes) return;
	if (!state.radio) { say('restore-status', 'Connect to the radio first (step 2).', 'bad'); return; }
	if (!confirm('Write this backup into the radio?\n\nThis replaces every channel, setting and the factory ' +
	             'calibration. Only do it with a backup taken from this same radio.')) return;

	$('btn-restore').disabled = true;
	say('restore-status', 'Writing…', 'busy');
	try {
		await state.radio.writeEeprom(restoreBytes, f => progress('restore-progress', f));
		await state.radio.reset();
		say('restore-status', 'Restored. The radio has been told to restart.', 'ok');
		log('EEPROM restore complete.');
	} catch (err) {
		say('restore-status', `The restore did not finish: ${err.message}`, 'bad');
		log(`Restore failed: ${err.message}`);
	} finally {
		$('btn-restore').disabled = false;
	}
}

// ------------------------------------------------------------------- init ---

async function init() {
	renderHistory();
	if (!checkSupport()) return;

	setStep(0);
	renderFirmwareCards(await loadManifest());

	on('fw-file', 'change', async (ev) => {
		const file = ev.target.files[0];
		if (!file) return;
		document.querySelectorAll('.fw').forEach(b => b.setAttribute('aria-pressed', 'false'));
		$('fw-file-status').textContent = `Checking ${file.name}…`;
		try {
			acceptImage(await readFile(file), file.name);
			$('fw-file-status').textContent = '';
		} catch (err) {
			$('fw-file-status').textContent = `Could not read that file: ${err.message}`;
		}
	});

	on('btn-connect',    'click', connect);
	on('btn-disconnect', 'click', disconnect);
	on('btn-backup',     'click', backup);
	on('btn-flash',      'click', flash);
	on('btn-identify',   'click', identify);
	on('btn-cable',      'click', checkCable);
	on('btn-loopback',   'click', testAdapter);
	on('btn-restore',    'click', restore);
	on('restore-file',   'change', pickRestoreFile);

	on('skip-backup', 'change', (ev) => {
		if (ev.target.checked) {
			say('backup-status', 'Backup skipped. Make sure you already have one saved somewhere safe.', 'bad');
			unlock(3);
		}
	});

	on('btn-copylog', 'click', async () => {
		try { await navigator.clipboard.writeText(logLines.join('\n')); log('Log copied to the clipboard.'); }
		catch { log('The browser would not let the page copy to the clipboard - select the text instead.'); }
	});

	on('btn-restart', 'click', () => {
		state.backupDone = false;
		state.flashed = false;
		['backup-status', 'flash-status', 'done-summary'].forEach(id => say(id, ''));
		['backup-progress', 'flash-progress'].forEach(id => { $(id).hidden = true; $(id).firstElementChild.style.width = '0'; });
		$('skip-backup').checked = false;
		setStep(1);
	});

	window.addEventListener('beforeunload', (ev) => {
		if ($('btn-flash').disabled && !state.flashed) { ev.preventDefault(); ev.returnValue = ''; }
	});

	log('Installer ready.');
}

init();
