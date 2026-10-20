/* Host test for app/alert_demod.c, end to end: synthesised ALERT bursts as
 * 12-bit ADC samples at 9600 Hz -> ALERT_DemodSample -> ALERT_ScanBitsGated,
 * decoded exactly as alert.c decodes a burst (both data senses, POL_ANY,
 * 12 idle bits ahead of a frame).
 *
 * The signal is what the off-air measurement in quansheng_alert_v3 found:
 * 300 baud, mark 2100 Hz, space 1300 Hz, continuous phase, four 10-bit async
 * words per reading with negative-logic framing behind an idle preamble. The
 * PRNG is fixed, so every run sees the same noise.
 *
 *   make -C test
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app/alert_decode.h"
#include "app/alert_demod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846   // not in strict C11
#endif

static int fails;

static void check(const char *what, bool cond)
{
	printf("%-66s %s\n", what, cond ? "pass" : "FAIL");
	if (!cond) fails++;
}

// ---- deterministic noise -----------------------------------------------------

static uint32_t rng = 0x12345678u;

static uint32_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static double gauss(void)
{
	const double u1 = ((xorshift() >> 8) + 1.0) / 16777217.0;
	const double u2 = (xorshift() >> 8) / 16777216.0;
	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

// ---- frames ------------------------------------------------------------------

// The 32 payload bits of an ABF reading, in transmission order (MegaNet packets.js).
static void abf_payload(uint16_t a, uint16_t d, uint8_t p[32])
{
	int n = 0;
	for (int i = 0; i < 6; i++) p[n++] = (a >> i) & 1;
	p[n++] = 1; p[n++] = 0;
	for (int i = 6; i < 12; i++) p[n++] = (a >> i) & 1;
	p[n++] = 1; p[n++] = 0;
	p[n++] = (a >> 12) & 1;
	for (int i = 0; i < 5; i++) p[n++] = (d >> i) & 1;
	p[n++] = 1; p[n++] = 1;
	for (int i = 5; i < 11; i++) p[n++] = (d >> i) & 1;
	p[n++] = 1; p[n++] = 1;
}

// Append one reading as four negative-logic async words (start 1, 8 data LSB first, stop 0).
static int put_frame(uint8_t *bits, int n, uint16_t id, uint16_t value)
{
	uint8_t p[32];
	abf_payload(id, value, p);
	for (int k = 0; k < 4; k++) {
		bits[n++] = 1;
		for (int i = 0; i < 8; i++) bits[n++] = p[8 * k + i];
		bits[n++] = 0;
	}
	return n;
}

static int put_idle(uint8_t *bits, int n, int count)
{
	while (count--) bits[n++] = 0;
	return n;
}

// ---- the radio's audio -------------------------------------------------------

typedef struct {
	double baud;        // bit rate the transmitter really sends
	double amp;         // tone amplitude, ADC counts
	double mark_db;     // extra gain on the 2100 Hz tone (de-emphasis is negative)
	double dc;          // where the pin sits, ADC counts
	double noise;       // noise sigma, ADC counts
	bool   invert;      // swap the tones
	int    lead;        // samples of noise-only audio before the tones start
} Sig_t;

static const Sig_t CLEAN = { 300.0, 400.0, 0.0, 2048.0, 0.0, false, 0 };

// Run one burst through the demodulator; returns the bit count.
static uint16_t demod(const uint8_t *bits, int nbits, const Sig_t *s)
{
	double ph = 0.0, t = 0.0;
	const double per_bit = ALERT_DEMOD_FS / s->baud;
	const double mark_gain = pow(10.0, s->mark_db / 20.0);

	ALERT_DemodReset();
	for (int i = 0; i < s->lead; i++) {
		long v = lround(s->dc + s->noise * gauss());
		ALERT_DemodSample(v < 0 ? 0 : v > 4095 ? 4095 : v);
	}
	for (;;) {
		const int k = (int)(t / per_bit);
		if (k >= nbits) break;
		const bool mark = (bits[k] != 0) != s->invert;
		const double f = mark ? 2100.0 : 1300.0;
		const double a = mark ? s->amp * mark_gain : s->amp;
		long v = lround(s->dc + a * sin(ph) + s->noise * gauss());
		ALERT_DemodSample(v < 0 ? 0 : v > 4095 ? 4095 : v);
		ph += 2.0 * M_PI * f / ALERT_DEMOD_FS;
		t += 1.0;
	}
	return ALERT_DemodBitCount();
}

// Decode the demodulated bits exactly as alert.c does after a burst.
static int decode(AlertReading_t *r, int max, int *inverted)
{
	const uint16_t n = ALERT_DemodBitCount();
	int na = ALERT_ScanBitsGated(ALERT_DemodBits(), n, ALERT_POL_ANY, 20, false, 12u, r, max);
	const int na0 = na;
	if (na < max)
		na += ALERT_ScanBitsGated(ALERT_DemodBits(), n, ALERT_POL_ANY, 20, true, 12u, r + na, max - na);
	if (inverted) *inverted = na - na0;
	return na;
}

static bool has(const AlertReading_t *r, int n, uint16_t id, uint16_t value)
{
	for (int i = 0; i < n; i++)
		if (r[i].id == id && r[i].value == value) return true;
	return false;
}

// One burst: a preamble, one reading, a tail.
static bool one(uint16_t id, uint16_t value, const Sig_t *s, int *inverted)
{
	uint8_t bits[200];
	AlertReading_t r[8];
	int n = put_idle(bits, 0, 60);
	n = put_frame(bits, n, id, value);
	n = put_idle(bits, n, 20);
	demod(bits, n, s);
	const int na = decode(r, 8, inverted);
	return has(r, na, id, value);
}

// How many of `count` bursts with random readings decode exactly.
static int rate(const Sig_t *s, int count)
{
	int ok = 0;
	for (int i = 0; i < count; i++) {
		const uint16_t id = (uint16_t)(xorshift() % 8192u), value = (uint16_t)(xorshift() % 2047u);
		ok += one(id, value, s, NULL);
	}
	return ok;
}

int main(void)
{
	char what[96];
	int inv;

	// 1. the plain case
	{
		inv = -1;
		check("clean burst, ABF id 6129 value 1599, decodes", one(6129, 1599, &CLEAN, &inv));
		check("... in the normal data sense", inv == 0);
		const uint16_t n = ALERT_DemodBitCount();
		snprintf(what, sizeof(what), "one bit per 32 samples: %u bits for 120 sent", n);
		check(what, n >= 118 && n <= 121);
	}

	// 2. two stations back to back, the second with no preamble of its own
	{
		uint8_t bits[240];
		AlertReading_t r[8];
		int n = put_idle(bits, 0, 60);
		n = put_frame(bits, n, 1234, 42);
		n = put_frame(bits, n, 4321, 2047);
		n = put_idle(bits, n, 20);
		demod(bits, n, &CLEAN);
		const int na = decode(r, 8, NULL);
		check("two readings back to back: both decode", has(r, na, 1234, 42) && has(r, na, 4321, 2047));
	}

	// 3. the transmitter's clock is off: the AL200X only promises "300 baud"
	{
		Sig_t s = CLEAN;
		s.baud = 300.0 * 1.015;
		check("bit rate 1.5% fast: 20 of 20 decode", rate(&s, 20) == 20);
		s.baud = 300.0 * 0.985;
		check("bit rate 1.5% slow: 20 of 20 decode", rate(&s, 20) == 20);
	}

	// 4. FM de-emphasis takes ~4 dB off 2100 Hz relative to 1300 Hz
	{
		Sig_t s = CLEAN;
		s.mark_db = -4.0;
		check("2100 Hz 4 dB down (de-emphasis): 20 of 20 decode", rate(&s, 20) == 20);
	}

	// 5. the other data sense, which only the complemented pass can read
	{
		Sig_t s = CLEAN;
		s.invert = true;
		inv = -1;
		check("tones swapped: still decodes", one(2784, 1599, &s, &inv));
		check("... through the inverted pass", inv >= 1);
	}

	// 6. where the pin sits: the DC tracker starts at the first sample
	{
		Sig_t s = CLEAN;
		s.dc = 700.0;
		check("pin sitting at 700 counts: 20 of 20 decode", rate(&s, 20) == 20);
		s.dc = 3400.0;
		check("pin sitting at 3400 counts: 20 of 20 decode", rate(&s, 20) == 20);
		s.dc = 2048.0;
		s.lead = 3000;              // 0.3 s of hiss before the tones: the squelch opened early
		s.noise = 40.0;
		check("0.3 s of hiss ahead of the burst: 20 of 20 decode", rate(&s, 20) == 20);
	}

	// 7. noise: the decode rate at an amplitude-to-noise ratio of 2 (6 dB), and at 4
	{
		Sig_t s = CLEAN;
		s.noise = s.amp / 4.0;
		const int a = rate(&s, 40);
		snprintf(what, sizeof(what), "noise sigma amp/4: %d of 40 decode (want >= 39)", a);
		check(what, a >= 39);
		s.noise = s.amp / 2.0;
		const int b = rate(&s, 40);
		snprintf(what, sizeof(what), "noise sigma amp/2: %d of 40 decode (want >= 30)", b);
		check(what, b >= 30);
	}

	// 8. no signal at all: the idle gate keeps chance decodes out
	{
		AlertReading_t r[8];
		int hits = 0;
		for (int i = 0; i < 200; i++) {
			ALERT_DemodReset();
			for (int k = 0; k < 14400; k++) {          // 1.5 s, the longest burst that qualifies
				long v = lround(2048.0 + 300.0 * gauss());
				ALERT_DemodSample(v < 0 ? 0 : v > 4095 ? 4095 : v);
			}
			hits += decode(r, 8, NULL);
		}
		snprintf(what, sizeof(what), "200 bursts of pure noise: %d chance decodes (want <= 2)", hits);
		check(what, hits <= 2);
	}

	// 9. the level report the raw view shows
	{
		uint16_t mn, mx, mean;
		Sig_t s = CLEAN;
		s.dc = 1000.0;
		s.amp = 300.0;
		one(100, 200, &s, NULL);
		ALERT_DemodLevels(&mn, &mx, &mean);
		snprintf(what, sizeof(what), "levels: min %u max %u mean %u around 1000 +/- 300", mn, mx, mean);
		check(what, mn >= 695 && mn <= 705 && mx >= 1295 && mx <= 1305 && mean >= 990 && mean <= 1010);
		uint8_t bits[40] = { 0 };
		Sig_t flat = CLEAN;
		flat.amp = 0.0;
		flat.dc = 4095.0;
		demod(bits, 40, &flat);
		ALERT_DemodLevels(&mn, &mx, &mean);
		check("a pin pinned at the rail reads 4095 / 4095", mn == 4095 && mx == 4095 && mean == 4095);
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all tests passed", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
