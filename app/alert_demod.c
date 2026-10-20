/* ALERT receiver: software AFSK demodulator - see alert_demod.h.
 *
 * Copyright 2026 cdomotor-g. Apache-2.0, like the egzumer base it lives in.
 */
#include <string.h>

#include "app/alert_demod.h"

#define ALERT_MARK_HZ  2100u
#define ALERT_SPACE_HZ 1300u
#define MARK_INC  ((256u * ALERT_MARK_HZ  + ALERT_DEMOD_FS / 2u) / ALERT_DEMOD_FS)   // 56 = 2100.0 Hz
#define SPACE_INC ((256u * ALERT_SPACE_HZ + ALERT_DEMOD_FS / 2u) / ALERT_DEMOD_FS)   // 35 = 1312.5 Hz
#define DEMOD_SPB (ALERT_DEMOD_FS / 300u)                                             // 32 samples per bit
#define CORR_W    16u                                                                 // correlator window, 1.67 ms

// quarter-wave-symmetric 32-entry sine, int8
static const int8_t sinTab[32] = {
	  0,  25,  49,  71,  90, 106, 117, 125, 127, 125, 117, 106,  90,  71,  49,  25,
	  0, -25, -49, -71, -90,-106,-117,-125,-127,-125,-117,-106, -90, -71, -49, -25
};

static inline int8_t sin256(uint8_t ph) { return sinTab[ph >> 3]; }
static inline int8_t cos256(uint8_t ph) { return sinTab[(uint8_t)(ph + 64u) >> 3]; }

static struct {
	int32_t  dc_x16;
	int32_t  s1c, s1s, s2c, s2s;
	int16_t  h1c[CORR_W], h1s[CORR_W], h2c[CORR_W], h2s[CORR_W];
	uint16_t nbits;
	uint8_t  ph1, ph2;              // tone phases, 1/256 cycle
	uint8_t  hi;
	uint8_t  phase;                 // bit clock phase 0..DEMOD_SPB-1
	uint8_t  lastBit;
	uint8_t  first;
	uint16_t mn, mx;                // raw level over the burst
	uint32_t sum, n;
	uint8_t  buf[ALERT_DEMOD_BYTES];
} dm;

void ALERT_DemodReset(void)
{
	memset(&dm, 0, sizeof(dm));
	dm.first = 1;
}

void ALERT_DemodSample(int32_t raw)
{
	if (dm.first) {
		dm.first  = 0;
		dm.dc_x16 = raw << 4;
		dm.mn = dm.mx = (uint16_t)raw;
	}
	if (raw < dm.mn) dm.mn = (uint16_t)raw;
	if (raw > dm.mx) dm.mx = (uint16_t)raw;
	if (dm.n < 0xFFFFFu) {          // 109 s of samples; a burst is 1.5 s at most
		dm.sum += (uint32_t)raw;
		dm.n++;
	}

	// DC removal (slow tracker) and scaling to ~8 bits
	dm.dc_x16 += ((raw << 4) - dm.dc_x16) >> 7;
	int32_t x = (raw - (dm.dc_x16 >> 4)) >> 3;

	// Two quadrature correlators with a sliding window, in int16. A 12-bit
	// sample can sit up to 4095 counts off the tracker (a step the ~13 ms DC
	// tracker has not caught up with yet), which would make |x| 511 and wrap
	// the products; clamped, 256 * 127 = 32512 fits.
	if (x > 255)
		x = 255;
	else if (x < -256)
		x = -256;
	const int16_t p1c = (int16_t)(x * cos256(dm.ph1)), p1s = (int16_t)(x * sin256(dm.ph1));
	const int16_t p2c = (int16_t)(x * cos256(dm.ph2)), p2s = (int16_t)(x * sin256(dm.ph2));
	dm.ph1 = (uint8_t)(dm.ph1 + MARK_INC);
	dm.ph2 = (uint8_t)(dm.ph2 + SPACE_INC);

	dm.s1c += p1c - dm.h1c[dm.hi]; dm.h1c[dm.hi] = p1c;
	dm.s1s += p1s - dm.h1s[dm.hi]; dm.h1s[dm.hi] = p1s;
	dm.s2c += p2c - dm.h2c[dm.hi]; dm.h2c[dm.hi] = p2c;
	dm.s2s += p2s - dm.h2s[dm.hi]; dm.h2s[dm.hi] = p2s;
	if (++dm.hi >= CORR_W) dm.hi = 0;

	const int32_t a1 = dm.s1c >> 6, b1 = dm.s1s >> 6, a2 = dm.s2c >> 6, b2 = dm.s2s >> 6;
	const uint8_t bit = ((a1 * a1 + b1 * b1) > (a2 * a2 + b2 * b2)) ? 1u : 0u;   // mark (2100 Hz) = 1

	// bit clock recovery: pull the phase towards the transitions
	if (bit != dm.lastBit) {
		if (dm.phase < DEMOD_SPB / 2u) dm.phase -= dm.phase / 4u;
		else                           dm.phase += (DEMOD_SPB - dm.phase) / 4u;
		dm.lastBit = bit;
	}

	if (++dm.phase >= DEMOD_SPB)
		dm.phase = 0;
	// Sample point: half a bit after the transitions the loop above locks to.
	// It used to be phase 0, on the reasoning that the correlator's delay put
	// that mid-bit - but the transitions the loop steers by come through the
	// very same correlator, so the delay cancels and phase 0 is the bit
	// boundary. The adjustments above only ever move the phase away from 16 in
	// the direction it is already going, so each bit is still taken once.
	if (dm.phase == DEMOD_SPB / 2u && dm.nbits < ALERT_DEMOD_BITS) {
		const uint8_t m = (uint8_t)(0x80u >> (dm.nbits & 7u));
		if (bit) dm.buf[dm.nbits >> 3] |= m;
		else     dm.buf[dm.nbits >> 3] &= (uint8_t)~m;
		dm.nbits++;
	}
}

uint16_t ALERT_DemodBitCount(void) { return dm.nbits; }
const uint8_t *ALERT_DemodBits(void) { return dm.buf; }

void ALERT_DemodLevels(uint16_t *min, uint16_t *max, uint16_t *mean)
{
	*min  = dm.mn;
	*max  = dm.mx;
	*mean = dm.n ? (uint16_t)(dm.sum / dm.n) : 0u;
}
