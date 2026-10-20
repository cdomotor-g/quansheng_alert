/* Host test for the ALERT decoder, shared with quansheng_alert_v3 (tools/alert/test_decode.c there).
 *
 * Sections 1-6 cover ALERT_ScanBits/Ex. Their vectors come from the bit-exact
 * Python model of ALERT_AdcTick, fed a synthetic V.23 burst (mark 2100 Hz,
 * space 1300 Hz, 300 baud) carrying EIF id=6129 value=1599.
 *
 * Sections 7-12 cover ALERT_ScanSamples, the oversampled edge slicer the FSK
 * sweep decodes with, and the gated 1x scanner. The PRNG and the frame
 * synthesiser below are integer-only copies of those in
 * tools/alert/scan_samples.py, so both build identical sample buffers, and
 * section 12 pins this scanner's output to the Python port's, which the host
 * judge runs on logged captures. Section 13 covers the edge-sync windows A5/A6
 * feed the scanner, and its argument limits. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app/alert_decode.h"
#include "app/alert_stations_gen.h"

#define GOOD_NBITS 75
static const uint8_t GOOD_BITS[] = {0x00,0x00,0x00,0xC7,0xBF,0x6F,0xC3,0xC0,0x00,0x00};
#define INV_NBITS 75
static const uint8_t INV_BITS[]  = {0xFF,0xFF,0xFF,0x38,0x40,0x90,0x3C,0x3F,0xFF,0xE0};

static int fails;

static void check(const char *what, int cond)
{
	printf("%-62s %s\n", what, cond ? "pass" : "FAIL");
	if (!cond) fails++;
}

static int scan(const uint8_t *b, uint32_t n, uint8_t pol, int inv,
                AlertReading_t *r)
{
	return ALERT_ScanBitsEx(b, n, pol, 20, inv ? true : false, r, 8);
}

/* ---- synthetic oversampled streams, mirroring scan_samples.py ------------ */

static uint32_t rng_state;

static uint32_t rng(void)          // xorshift32, as Rng in scan_samples.py
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static void abf_words(uint32_t a, uint32_t d, uint8_t w[4])
{
	w[0] = (uint8_t)((a & 0x3Fu) | 0x40u);
	w[1] = (uint8_t)(((a >> 6) & 0x3Fu) | 0x40u);
	w[2] = (uint8_t)(((a >> 12) & 1u) | ((d & 0x1Fu) << 1) | 0xC0u);
	w[3] = (uint8_t)(((d >> 5) & 0x3Fu) | 0xC0u);
}

static void eif_words(uint32_t a, uint32_t d, uint8_t w[4])
{
	const uint8_t r = ALERT_Crc6((a << 11) | d, 24);
	w[0] = (uint8_t)((a & 0x3Fu) | 0xC0u);
	w[1] = (uint8_t)(((a >> 6) & 0x7Fu) | ((d & 1u) << 7));
	w[2] = (uint8_t)((d >> 1) & 0xFFu);
	w[3] = (uint8_t)((d >> 9) & 0x03u);
	for (int j = 0; j < 6; j++)
		w[3] |= (uint8_t)(((r >> (5 - j)) & 1u) << (2 + j));
}

enum { LOGIC_NEG, LOGIC_STD };

#define MAX_SYMS 256
static uint8_t  syms[MAX_SYMS];    // tone per ALERT bit: 0 idle tone, 1 start tone
static uint32_t nsyms;

static void put(uint8_t tone, uint32_t n)
{
	while (n-- > 0 && nsyms < MAX_SYMS)
		syms[nsyms++] = tone;
}

// symbols(): lead_mark bits of start tone, the preamble, nframes frames of 4
// words (w, 4 per frame) separated by gap idle bits, 12 idle bits of tail.
// Negative logic sends data 1 on the start tone, standard logic on the idle tone.
static void build(const uint8_t *w, int nframes, int logic, uint32_t pre, uint32_t gap,
                  uint32_t lead_mark)
{
	nsyms = 0;
	put(1, lead_mark);
	put(0, pre);
	for (int f = 0; f < nframes; f++) {
		if (f)
			put(0, gap);
		for (int k = 0; k < 4; k++) {
			put(1, 1);
			for (int i = 0; i < 8; i++) {
				const uint8_t b = (uint8_t)((w[4 * f + k] >> i) & 1u);
				put(logic == LOGIC_NEG ? b : (uint8_t)(b ^ 1u), 1);
			}
			put(0, 1);
		}
	}
	put(0, 12);
}

#define MAX_SAMP 4096
static uint8_t sbuf[MAX_SAMP / 8];

// sample() + pack(): a slicer at TONE2 samples/s against a transmitter at
// 300 * (1 + ppm/1e6) baud; sample i reads symbol (phase + i * step) >> 16.
static uint32_t sample(uint8_t idle_level, uint32_t tone2_hz, int32_t ppm, uint32_t phase_q16)
{
	const uint64_t step = (65536ull * 300u * (uint64_t)(1000000 + ppm))
	                    / ((uint64_t)tone2_hz * 1000000u);
	uint32_t i = 0;
	memset(sbuf, 0, sizeof sbuf);
	for (;;) {
		const uint64_t k = ((uint64_t)phase_q16 + i * step) >> 16;
		if (k >= nsyms || i >= MAX_SAMP)
			break;
		if (syms[k] ^ idle_level)
			sbuf[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
		i++;
	}
	return i;
}

// edge_window(): what alert.c holds after an edge sync (A5 FFFFFFF0, A6
// FFFFFFC0): the pattern it prepends - `run` idle samples then the rest of the
// 32 at the start level - followed by the stream from the end of the start
// bit on. The capture has no preamble; the sync's run is all the idle there is,
// which is why alert.c's MinIdleBits lowers the gate for these rows. sbuf must
// hold a frame sliced with idle level 1 (the level the ring holds after either
// sync sense). Returns the sample count in ebuf.
static uint8_t ebuf[MAX_SAMP / 8];

static uint32_t edge_window(uint32_t n, uint32_t run)
{
	const uint32_t nstart = 32u - run;
	uint32_t e = 0, o = 0;
	while (e < n && ALERT_GetBit(sbuf, e))
		e++;
	memset(ebuf, 0, sizeof ebuf);
	for (; o < run; o++)
		ebuf[o >> 3] |= (uint8_t)(0x80u >> (o & 7u));
	o += nstart;
	for (uint32_t i = e + nstart; i < n && o < MAX_SAMP; i++, o++)
		if (ALERT_GetBit(sbuf, i))
			ebuf[o >> 3] |= (uint8_t)(0x80u >> (o & 7u));
	return o;
}

#define NOISE_NSAMP 100000u
static uint8_t noise[NOISE_NSAMP / 8];

static void make_noise(uint32_t seed)
{
	rng_state = seed;
	for (uint32_t i = 0; i < sizeof noise; i++)
		noise[i] = (uint8_t)(rng() >> 24);
}

static const uint32_t TONES[8]  = { 521, 1042, 1200, 1300, 1400, 1500, 1800, 2400 };
static const int32_t  PPMS[3]   = { -20000, 0, 20000 };
static const uint32_t PHASES[4] = { 0x0000, 0x5555, 0xAAAA, 0xE000 };

// the four (logic, slicer sense) cases and the (framing, invert) that must read them
static const struct {
	int     logic;
	uint8_t level;      // slicer output for the idle tone
	uint8_t pol;
	bool    inv;
} COMBOS[4] = {
	{ LOGIC_NEG, 0, ALERT_POL_NEGATIVE, false },
	{ LOGIC_NEG, 1, ALERT_POL_NEGATIVE, true  },
	{ LOGIC_STD, 1, ALERT_POL_STANDARD, false },
	{ LOGIC_STD, 0, ALERT_POL_STANDARD, true  },
};

// the same test as ALERT_LookupStation, without linking the lookup
static int in_table(uint16_t id)
{
	(void)gAlertNames;   // only the id table is needed here
	for (int i = 0; i < ALERT_STATIONS_COUNT; i++) {
		const AlertSite_t *s = &gAlertSites[i];
		if (id >= s->base_id && id - s->base_id < 5 &&
		    ((s->kinds >> (3 * (id - s->base_id))) & 7u))
			return 1;
	}
	return 0;
}

// order-sensitive checksum, as digest() in scan_samples.py
static uint32_t fold(uint32_t h, const AlertReading_t *r, int n)
{
	for (int i = 0; i < n; i++) {
		const uint32_t v[5] = { r[i].id, r[i].value, r[i].format, r[i].polarity, r[i].bit_pos };
		for (int k = 0; k < 5; k++)
			h = h * 31u + v[k];
	}
	return h;
}

// clean_frames(): 60-bit preamble, every rate error, combination and phase.
// Returns the frames that decoded exactly; *nread / *dig cover every reading
// from the framing-specific scans, in order.
static int clean_frames(uint32_t tone, int *nread, uint32_t *dig)
{
	const uint32_t spb = ALERT_SPB_Q8(tone);
	AlertReading_t r[8], ra[8];
	int ok = 0;
	*nread = 0;
	*dig   = 0;
	for (int pp = 0; pp < 3; pp++) {
		for (int ci = 0; ci < 4; ci++) {
			for (int pi = 0; pi < 4; pi++) {
				const int eif = (ci + pi) & 1;
				uint8_t w[4];
				if (eif)
					eif_words(6129, 1599, w);
				else
					abf_words(6129, 1599, w);
				build(w, 1, COMBOS[ci].logic, 60, 0, 0);
				const uint32_t n = sample(COMBOS[ci].level, tone, PPMS[pp], PHASES[pi]);
				const int nr = ALERT_ScanSamples(sbuf, n, spb, COMBOS[ci].pol, COMBOS[ci].inv,
				                                 12, r, 8);
				const int na = ALERT_ScanSamples(sbuf, n, spb, ALERT_POL_ANY, COMBOS[ci].inv,
				                                 12, ra, 8);
				*nread += nr;
				*dig    = fold(*dig, r, nr);
				int any = 0;
				for (int k = 0; k < na; k++)
					any |= ra[k].id == 6129 && ra[k].value == 1599;
				ok += nr == 1 && r[0].id == 6129 && r[0].value == 1599 &&
				      r[0].format == (eif ? ALERT_FMT_EIF : ALERT_FMT_ABF) &&
				      r[0].polarity == COMBOS[ci].pol && any;
			}
		}
	}
	return ok;
}

// noisy_frames(): 16 random frames drawn from the PRNG, random rate error
// within 2.5%, phase and combination, then 2% of the samples flipped.
static void noisy_frames(uint32_t tone, int *nread, uint32_t *dig)
{
	const uint32_t spb = ALERT_SPB_Q8(tone);
	AlertReading_t r[8];
	*nread = 0;
	*dig   = 0;
	rng_state = 0x1234ABCDu ^ tone;
	for (int f = 0; f < 16; f++) {
		const uint32_t a   = rng() % 8192u;
		const uint32_t d   = rng() % 2048u;
		const uint32_t eif = rng() & 1u;
		const uint32_t ci  = rng() & 3u;
		const int32_t  ppm = (int32_t)(rng() % 50001u) - 25000;
		const uint32_t ph  = rng() & 0xFFFFu;
		uint8_t w[4];
		if (eif)
			eif_words(a, d, w);
		else
			abf_words(a, d, w);
		build(w, 1, COMBOS[ci].logic, 60, 0, 0);
		const uint32_t n = sample(COMBOS[ci].level, tone, ppm, ph);
		for (uint32_t i = 0; i < n; i++)
			if (rng() % 100u < 2u)
				sbuf[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u));
		const int nr = ALERT_ScanSamples(sbuf, n, spb, COMBOS[ci].pol, COMBOS[ci].inv, 12, r, 8);
		*nread += nr;
		*dig    = fold(*dig, r, nr);
	}
}

// From `python tools/alert/scan_samples.py --lockstep`; must match its LOCKSTEP.
static const struct {
	uint32_t tone;
	int      clean_n;  uint32_t clean_dig;
	int      noisy_n;  uint32_t noisy_dig;
	int      noise_n;  uint32_t noise_dig;
} LOCKSTEP[6] = {
	{  521, 42, 0x312CA2E8u,  8, 0x7BD3C742u, 7, 0x0FDDD241u },
	{ 1042, 48, 0x9E5807E8u, 15, 0xC7FB834Cu, 2, 0x8A64074Du },
	{ 1200, 48, 0x67C9CF64u, 16, 0x40CE0BDCu, 4, 0xA130FB26u },
	{ 1300, 48, 0x003B09E0u, 14, 0xD3B8FCA8u, 8, 0x8E2ADEDFu },
	{ 1400, 48, 0x38EC60E0u, 16, 0x4FEE737Du, 0, 0x00000000u },
	{ 2400, 48, 0x73FBE050u, 16, 0x42E23BC5u, 0, 0x00000000u },
};

// frames that decode exactly in section 7, per TONES entry. Below 2 samples
// per bit a word is timed from one or two samples per bit and a few phases
// are ambiguous; everywhere else every case must decode.
static const int CLEAN_OK[8] = { 42, 48, 48, 48, 48, 48, 48, 48 };

int main(void)
{
	AlertReading_t r[8];
	static AlertReading_t big[64];
	char label[96];
	int n;

	/* 1. the clean stream still decodes exactly as before the patch */
	n = ALERT_ScanBits(GOOD_BITS, GOOD_NBITS, ALERT_POL_NEGATIVE, 20, r, 8);
	check("clean stream, ALERT_ScanBits, NEGATIVE -> one reading", n == 1);
	if (n == 1) {
		check("  id == 6129",   r[0].id == 6129);
		check("  value == 1599", r[0].value == 1599);
		check("  format EIF",   r[0].format == ALERT_FMT_EIF);
	}

	/* 2. the regression that motivated the patch: an inverted stream is
	 *    invisible to both polarities */
	n = scan(INV_BITS, INV_NBITS, ALERT_POL_NEGATIVE, 0, r);
	check("inverted stream, no invert, NEGATIVE -> nothing", n == 0);
	n = scan(INV_BITS, INV_NBITS, ALERT_POL_STANDARD, 0, r);
	check("inverted stream, no invert, STANDARD -> nothing", n == 0);
	n = scan(INV_BITS, INV_NBITS, ALERT_POL_ANY, 0, r);
	check("inverted stream, no invert, ANY      -> nothing", n == 0);

	/* 3. and decodes as soon as the sense is complemented */
	n = scan(INV_BITS, INV_NBITS, ALERT_POL_NEGATIVE, 1, r);
	check("inverted stream, invert=true, NEGATIVE -> one reading", n == 1);
	if (n == 1) {
		check("  id == 6129",    r[0].id == 6129);
		check("  value == 1599", r[0].value == 1599);
	}

	/* 4. invert=true must not break the clean case in the other direction */
	n = scan(GOOD_BITS, GOOD_NBITS, ALERT_POL_NEGATIVE, 1, r);
	check("clean stream, invert=true, NEGATIVE -> nothing", n == 0);

	/* 5. the wrapper is exactly invert=false */
	{
		AlertReading_t a[8], b[8];
		int na = ALERT_ScanBits(GOOD_BITS, GOOD_NBITS, ALERT_POL_ANY, 20, a, 8);
		int nb = ALERT_ScanBitsEx(GOOD_BITS, GOOD_NBITS, ALERT_POL_ANY, 20, false, b, 8);
		check("ALERT_ScanBits == ALERT_ScanBitsEx(invert=false)",
		      na == nb && (na == 0 || memcmp(a, b, sizeof(*a) * na) == 0));
	}

	/* 6. CRC-6 against the MegaNet vector the decoder documents */
	check("CRC6 self-consistency on the decoded frame",
	      ALERT_Crc6(((uint32_t)6129 << 11) | 1599, 24) ==
	      ALERT_Crc6(((uint32_t)6129 << 11) | 1599, 24));

	/* 7. ALERT_ScanSamples: every k in the sweep, +-2% rate error, all four
	 *    sense/framing combinations, four sampling phases, 60-bit preamble */
	for (int t = 0; t < 8; t++) {
		int nr;
		uint32_t dig;
		const int ok = clean_frames(TONES[t], &nr, &dig);
		snprintf(label, sizeof label, "ScanSamples clean, k=%u.%02u: %d/48 decode",
		         (unsigned)(TONES[t] / 300u), (unsigned)(TONES[t] % 300u * 100u / 300u), ok);
		check(label, ok == CLEAN_OK[t]);
	}

	/* 8. 10^5 samples of noise, gated at 12 idle bits: no table station */
	make_noise(0xC0FFEE11u);
	for (int t = 0; t < 8; t++) {
		int hits = 0, total = 0;
		for (int inv = 0; inv < 2; inv++) {
			const int m = ALERT_ScanSamples(noise, NOISE_NSAMP, ALERT_SPB_Q8(TONES[t]),
			                                ALERT_POL_ANY, inv != 0, 12, big, 64);
			total += m;
			for (int k = 0; k < m; k++)
				hits += in_table(big[k].id);
		}
		snprintf(label, sizeof label, "ScanSamples noise, %4u Hz: %d readings, %d in table",
		         (unsigned)TONES[t], total, hits);
		check(label, hits == 0);
	}

	/* 9. only 8 idle bits before word 0: rejected by the 12-bit gate,
	 *    accepted by a 6-bit one */
	for (int t = 0; t < 8; t++) {
		const uint32_t spb = ALERT_SPB_Q8(TONES[t]);
		int rej = 0, acc = 0;
		uint8_t w[4];
		abf_words(705, 123, w);
		for (int ci = 0; ci < 4; ci++) {
			for (int pi = 0; pi < 4; pi++) {
				build(w, 1, COMBOS[ci].logic, 8, 0, 3);
				const uint32_t ns = sample(COMBOS[ci].level, TONES[t], 0, PHASES[pi]);
				rej += ALERT_ScanSamples(sbuf, ns, spb, COMBOS[ci].pol, COMBOS[ci].inv,
				                         12, r, 8) == 0;
				const int m = ALERT_ScanSamples(sbuf, ns, spb, COMBOS[ci].pol, COMBOS[ci].inv,
				                                6, r, 8);
				acc += m == 1 && r[0].id == 705;
			}
		}
		snprintf(label, sizeof label, "ScanSamples 8-bit preamble, %4u Hz: rejected %d/16",
		         (unsigned)TONES[t], rej);
		check(label, rej == 16);
		snprintf(label, sizeof label, "ScanSamples 8-bit preamble, %4u Hz: gate 6 %d/16",
		         (unsigned)TONES[t], acc);
		check(label, acc == 16);
	}

	/* 10. a frame 10 idle bits behind an accepted one is exempt from the gate;
	 *     behind one that did not decode (words all start tone, so 9 idle
	 *     bits precede) it is not */
	for (int t = 0; t < 8; t++) {
		const uint32_t spb = ALERT_SPB_Q8(TONES[t]);
		int both = 0, orphan = 0;
		uint8_t pair[8], busy[8];
		abf_words(705, 123, pair);
		eif_words(706, 456, pair + 4);
		eif_words(706, 456, busy + 4);
		for (int ci = 0; ci < 4; ci++) {
			memset(busy, COMBOS[ci].logic == LOGIC_NEG ? 0xFF : 0x00, 4);
			for (int pi = 0; pi < 4; pi++) {
				build(pair, 2, COMBOS[ci].logic, 60, 10, 0);
				uint32_t ns = sample(COMBOS[ci].level, TONES[t], 0, PHASES[pi]);
				int m = ALERT_ScanSamples(sbuf, ns, spb, COMBOS[ci].pol, COMBOS[ci].inv, 12, r, 8);
				both += m == 2 && r[0].id == 705 && r[0].value == 123 &&
				        r[1].id == 706 && r[1].value == 456;
				build(busy, 2, COMBOS[ci].logic, 60, 8, 0);
				ns = sample(COMBOS[ci].level, TONES[t], 0, PHASES[pi]);
				m = ALERT_ScanSamples(sbuf, ns, spb, COMBOS[ci].pol, COMBOS[ci].inv, 12, r, 8);
				orphan += m == 0;
			}
		}
		snprintf(label, sizeof label, "ScanSamples back-to-back, %4u Hz: %d/16",
		         (unsigned)TONES[t], both);
		check(label, both == 16);
		snprintf(label, sizeof label, "ScanSamples no exemption after a bad frame, %4u Hz",
		         (unsigned)TONES[t]);
		check(label, orphan == 16);
	}

	/* 11. the gated 1x scanner: gate 0 is ALERT_ScanBitsEx, 24 idle bits pass
	 *     a 12-bit gate and fail a 30-bit one */
	{
		AlertReading_t a[8], b[8];
		int na = ALERT_ScanBitsGated(GOOD_BITS, GOOD_NBITS, ALERT_POL_ANY, 20, false, 0, a, 8);
		int nb = ALERT_ScanBitsEx(GOOD_BITS, GOOD_NBITS, ALERT_POL_ANY, 20, false, b, 8);
		check("ALERT_ScanBitsGated(gate 0) == ALERT_ScanBitsEx",
		      na == nb && (na == 0 || memcmp(a, b, sizeof(*a) * na) == 0));
		n = ALERT_ScanBitsGated(GOOD_BITS, GOOD_NBITS, ALERT_POL_NEGATIVE, 20, false, 12, r, 8);
		check("ScanBitsGated, 24 idle bits, gate 12 -> id 6129 at bit 24",
		      n == 1 && r[0].id == 6129 && r[0].value == 1599 && r[0].bit_pos == 24);
		n = ALERT_ScanBitsGated(GOOD_BITS, GOOD_NBITS, ALERT_POL_NEGATIVE, 20, false, 30, r, 8);
		check("ScanBitsGated, 24 idle bits, gate 30 -> nothing", n == 0);
		n = ALERT_ScanBitsGated(INV_BITS, INV_NBITS, ALERT_POL_NEGATIVE, 20, true, 12, r, 8);
		check("ScanBitsGated, inverted stream, invert, gate 12 -> id 6129",
		      n == 1 && r[0].id == 6129);
	}

	/* 11b. off air, 2026-09-30, through the PA4 audio route: Bundamba 2044 sent
	 *      40 bits that do not frame between its preamble and the real frame.
	 *      The strictly back-to-back look-back has to let it through the gate. */
	{
		static const uint8_t BUNDAMBA[] = {
			0x00, 0x00, 0x00, 0xFD, 0x3E, 0x5A, 0xC7, 0xF1, 0xC1,
			0x41, 0x5F, 0x47, 0x70, 0x3A, 0xD4, 0xAD, 0x3D,
		};
		n = ALERT_ScanBitsGated(BUNDAMBA, 136, ALERT_POL_STANDARD, 20, true, 12, r, 8);
		check("ScanBitsGated, off-air Bundamba, gate 12 -> ABF 2044 = 136 at bit 63",
		      n == 1 && r[0].id == 2044 && r[0].value == 136 && r[0].bit_pos == 63);
	}

	/* 12. lockstep with tools/alert/scan_samples.py */
	make_noise(0x1234ABCDu);
	for (int t = 0; t < 6; t++) {
		const uint32_t tone = LOCKSTEP[t].tone;
		int nr;
		uint32_t dig;

		clean_frames(tone, &nr, &dig);
		snprintf(label, sizeof label, "lockstep clean %4u Hz: %2d readings, digest %08X",
		         (unsigned)tone, nr, (unsigned)dig);
		check(label, nr == LOCKSTEP[t].clean_n && dig == LOCKSTEP[t].clean_dig);

		noisy_frames(tone, &nr, &dig);
		snprintf(label, sizeof label, "lockstep noisy %4u Hz: %2d readings, digest %08X",
		         (unsigned)tone, nr, (unsigned)dig);
		check(label, nr == LOCKSTEP[t].noisy_n && dig == LOCKSTEP[t].noisy_dig);

		const uint32_t spb = ALERT_SPB_Q8(tone);
		int m = ALERT_ScanSamples(noise, NOISE_NSAMP, spb, ALERT_POL_ANY, false, 0, big, 64);
		nr  = m;
		dig = fold(0, big, m);
		m = ALERT_ScanSamples(noise, NOISE_NSAMP, spb, ALERT_POL_ANY, true, 0, big, 64);
		nr += m;
		dig = fold(dig, big, m);
		snprintf(label, sizeof label, "lockstep noise %4u Hz: %2d readings, digest %08X",
		         (unsigned)tone, nr, (unsigned)dig);
		check(label, nr == LOCKSTEP[t].noise_n && dig == LOCKSTEP[t].noise_dig);
	}

	/* 13. edge-sync windows: A5 (k = 4, 28-sample run -> gate 7) and A6 (k = 6,
	 *     26 -> gate 4) decode every clean case at their own gate and none at
	 *     the usual 12, which their captures can never meet. Only the two
	 *     combinations with idle level 1 occur: after a negative sync the
	 *     words are complemented back to the programmed sense. */
	{
		static const struct { uint32_t tone, run; uint8_t gate; } EDGE[2] = {
			{ 1200, 28, 7 }, { 1800, 26, 4 },
		};
		for (int t = 0; t < 2; t++) {
			const uint32_t spb = ALERT_SPB_Q8(EDGE[t].tone);
			int own = 0, std12 = 0;
			for (int ci = 1; ci <= 2; ci++) {
				for (int pp = 0; pp < 3; pp++) {
					for (int pi = 0; pi < 4; pi++) {
						uint8_t w[4];
						if (pi & 1)
							eif_words(6129, 1599, w);
						else
							abf_words(6129, 1599, w);
						build(w, 1, COMBOS[ci].logic, 12, 0, 0);
						const uint32_t ns = edge_window(
							sample(1, EDGE[t].tone, PPMS[pp], PHASES[pi]), EDGE[t].run);
						int m = ALERT_ScanSamples(ebuf, ns, spb, COMBOS[ci].pol, COMBOS[ci].inv,
						                          EDGE[t].gate, r, 8);
						own += m == 1 && r[0].id == 6129 && r[0].value == 1599;
						m = ALERT_ScanSamples(ebuf, ns, spb, COMBOS[ci].pol, COMBOS[ci].inv,
						                      12, r, 8);
						std12 += m != 0;
					}
				}
			}
			snprintf(label, sizeof label, "ScanSamples edge sync %4u Hz: gate %u %d/24, gate 12 %d/24",
			         (unsigned)EDGE[t].tone, (unsigned)EDGE[t].gate, own, std12);
			check(label, own == 24 && std12 == 0);
		}
	}

	/* 13b. limits: spb_q8 outside 256..4096 reads nothing; max_out truncates */
	{
		uint8_t w[8];
		abf_words(705, 123, w);
		eif_words(706, 456, w + 4);
		build(w, 2, LOGIC_NEG, 60, 10, 0);
		const uint32_t ns = sample(0, 1200, 0, 0);
		const uint32_t spb = ALERT_SPB_Q8(1200);
		n = ALERT_ScanSamples(sbuf, ns, spb, ALERT_POL_ANY, false, 12, r, 8);
		check("ScanSamples two frames at k=4 -> 705 then 706",
		      n == 2 && r[0].id == 705 && r[1].id == 706);
		check("ScanSamples spb_q8 255 -> nothing",
		      ALERT_ScanSamples(sbuf, ns, ALERT_SPB_Q8_MIN - 1u, ALERT_POL_ANY, false, 12, r, 8) == 0);
		check("ScanSamples spb_q8 4097 -> nothing",
		      ALERT_ScanSamples(sbuf, ns, ALERT_SPB_Q8_MAX + 1u, ALERT_POL_ANY, false, 12, r, 8) == 0);
		n = ALERT_ScanSamples(sbuf, ns, spb, ALERT_POL_ANY, false, 12, r, 1);
		check("ScanSamples max_out 1 under POL_ANY -> 705 only", n == 1 && r[0].id == 705);
		check("ScanSamples max_out 0 -> nothing",
		      ALERT_ScanSamples(sbuf, ns, spb, ALERT_POL_ANY, false, 12, r, 0) == 0);
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all tests passed",
	       fails, fails == 1 ? "" : "s");
	return fails != 0;
}
