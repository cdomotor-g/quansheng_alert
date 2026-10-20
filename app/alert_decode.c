/* ALERT (ERTS) telemetry frame decoder - see alert_decode.h.
 *
 * Reference: MegaNet packets.js (cdomotor-g/MegaNet), whose FORMATS table this
 * mirrors bit for bit. Payload bit positions are numbered 0..31 in
 * transmission order: word k data bit i (LSB first) is position 8k + i.
 *
 *   ABF   A0..A5 [1][0]   A6..A11 [1][0]   A12 D0..D4 [1][1]   D5..D10 [1][1]
 *   EIF   A0..A5 [1][1]   A6..A12 D0       D1..D8              D9 D10 R5..R0
 *
 * Address and data bits are LSB first; the EIF FCS is MSB first.
 */
#include "app/alert_decode.h"

#define CRC6_POLY 0x19u   // x^6 + x^4 + x^3 + 1

uint8_t ALERT_Crc6(uint32_t bits, uint8_t nbits)
{
	uint8_t reg = 0;
	for (int i = nbits - 1; i >= 0; i--) {
		const uint8_t b  = (bits >> i) & 1u;
		const uint8_t fb = ((reg >> 5) & 1u) ^ b;
		reg = (reg << 1) & 0x3fu;
		if (fb)
			reg ^= CRC6_POLY;
	}
	return reg;
}

// payload bit at transmission position p (0 = first sent)
#define P(p) ((payload >> (31u - (p))) & 1u)

// gather n bits starting at position p, LSB first
static uint16_t lsb_first(uint32_t payload, uint8_t p, uint8_t n)
{
	uint16_t v = 0;
	for (uint8_t i = 0; i < n; i++)
		v |= (uint16_t)P(p + i) << i;
	return v;
}

// gather n bits starting at position p, MSB first
static uint16_t msb_first(uint32_t payload, uint8_t p, uint8_t n)
{
	uint16_t v = 0;
	for (uint8_t i = 0; i < n; i++)
		v = (v << 1) | P(p + i);
	return v;
}

uint8_t ALERT_DecodePayload32(uint32_t payload, AlertReading_t *out)
{
	const uint8_t k1 = (P(6) << 1) | P(7);      // word 1 check bits
	const uint8_t k2 = (P(14) << 1) | P(15);    // word 2 check bits

	out->payload = payload;                     // kept for the record, decoded or not

	if (k1 == 2u && k2 == 2u) {
		// ABF: words 3 and 4 carry check bits 11
		if (P(22) && P(23) && P(30) && P(31)) {
			const uint16_t a = lsb_first(payload, 0, 6)
			                 | (lsb_first(payload, 8, 6) << 6)
			                 | (lsb_first(payload, 16, 1) << 12);
			const uint16_t d = lsb_first(payload, 17, 5)
			                 | (lsb_first(payload, 24, 6) << 5);
			out->id     = a;
			out->value  = d;
			out->format = ALERT_FMT_ABF;
			return ALERT_FMT_ABF;
		}
		return ALERT_FMT_NONE;
	}

	if (k1 == 3u) {
		// EIF: A0..A5 | A6..A12 D0 | D1..D8 | D9 D10 R5..R0
		const uint16_t a = lsb_first(payload, 0, 6)
		                 | (lsb_first(payload, 8, 7) << 6);
		const uint16_t d = lsb_first(payload, 15, 1)
		                 | (lsb_first(payload, 16, 8) << 1)
		                 | (lsb_first(payload, 24, 2) << 9);
		const uint8_t r  = (uint8_t)msb_first(payload, 26, 6);
		const uint32_t crc_in = ((uint32_t)a << 11) | d;   // address then data, MSB first
		if (ALERT_Crc6(crc_in, 24) == r) {
			out->id     = a;
			out->value  = d;
			out->format = ALERT_FMT_EIF;
			return ALERT_FMT_EIF;
		}
	}

	return ALERT_FMT_NONE;
}

bool ALERT_DecodeA2C(const uint8_t b[4], AlertReading_t *out)
{
	if (b[3] != 0)
		return false;
	out->id       = ((uint16_t)(b[1] & 0x1fu) << 8) | b[0];
	out->value    = ((uint16_t)(b[1] >> 5) << 8) | b[2];
	out->format   = ALERT_FMT_A2C;
	out->polarity = ALERT_POL_STANDARD;
	out->bit_pos  = 0;
	out->payload  = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
	return true;
}

// Payload in transmission order: position 8k+i = data bit i of word k.
static uint32_t assemble(const uint8_t words[4])
{
	uint32_t payload = 0;
	for (uint8_t k = 0; k < 4; k++)
		for (uint8_t i = 0; i < 8; i++)
			payload |= (uint32_t)((words[k] >> i) & 1u) << (31u - (8u * k + i));
	return payload;
}

// A frame that starts this close behind an accepted one skips the idle gate:
// a station may send a second frame straight after the first, and that one
// has no preamble of its own.
#define SCAN_EXEMPT_BITS 20u

// How far behind the end of an idle run a strictly back-to-back frame may start.
#define SCAN_LOOKBACK_BITS 48u

static int scan_polarity(const uint8_t *buf, uint32_t nbits, uint8_t polarity,
                         uint8_t max_gap, uint8_t inv, uint8_t min_idle_bits,
                         AlertReading_t *out, int max_out)
{
	const uint8_t idle  = (polarity == ALERT_POL_NEGATIVE) ? 0u : 1u;
	const uint8_t start = idle ^ 1u;
#define GB(n) (ALERT_GetBit(buf, (n)) ^ inv)

	uint8_t  words[4];
	uint32_t word_pos[4];
	uint8_t  nwords    = 0;
	int      found     = 0;
	uint32_t pos       = 0;
	uint32_t last_stop = 0;       // bit after the last accepted frame's final stop bit
	bool     have_last = false;

	while (pos + 10u <= nbits && found < max_out) {
		if (GB(pos) != start) {
			pos++;
			continue;
		}
		// a real start bit follows an idle bit (the previous stop bit or preamble)
		if (pos > 0 && GB(pos - 1u) != idle) {
			pos++;
			continue;
		}
		// candidate word: start bit at pos, stop bit at pos+9
		if (GB(pos + 9u) != idle) {
			pos++;               // framing error: slip one bit
			nwords = 0;
			continue;
		}
		uint8_t w = 0;
		for (uint8_t i = 0; i < 8; i++)
			w |= (uint8_t)(GB(pos + 1u + i) << i);   // LSB first

		// drop the run if this word does not follow the previous one closely
		if (nwords && (pos - (word_pos[nwords - 1] + 10u)) > max_gap)
			nwords = 0;

		if (nwords == 4) {
			words[0] = words[1]; words[1] = words[2]; words[2] = words[3];
			word_pos[0] = word_pos[1]; word_pos[1] = word_pos[2]; word_pos[2] = word_pos[3];
			nwords = 3;
		}
		words[nwords]    = w;
		word_pos[nwords] = pos;
		nwords++;
		pos += 10u;

		if (nwords == 4) {
			// ABF and EIF each pin down only about 8 bits, so on noise the
			// four-word test alone passes far too often. A real frame follows
			// a preamble: demand min_idle_bits of idle right before word 0.
			const uint32_t p0 = word_pos[0];
			bool gate = min_idle_bits == 0 ||
			            (have_last && p0 <= last_stop + SCAN_EXEMPT_BITS);
			if (!gate && p0 >= min_idle_bits) {
				uint32_t j = p0 - min_idle_bits;
				while (j < p0 && GB(j) == idle)
					j++;
				gate = (j == p0);
			}
			// A strictly back-to-back frame may also sit a little way behind the
			// preamble. Seen on air from Bundamba 2044: 40 bits of something that
			// does not frame, then the real frame, no idle between. Real ALERT
			// words go out back to back, and demanding that here keeps the chance
			// rate on noise where the plain gate has it (0.04% of 136-bit
			// captures, measured through the Python port, against 0.29% for the
			// same look-back with the usual 20-bit word gap).
			if (!gate && min_idle_bits &&
			    word_pos[1] == p0 + 10u && word_pos[2] == p0 + 20u && word_pos[3] == p0 + 30u) {
				const uint32_t lo = (p0 > SCAN_LOOKBACK_BITS + min_idle_bits)
				                  ? p0 - SCAN_LOOKBACK_BITS - min_idle_bits : 0u;
				uint32_t run = 0;
				for (uint32_t k = lo; k < p0 && !gate; k++) {
					run = (GB(k) == idle) ? run + 1u : 0u;
					gate = run >= min_idle_bits && k + 1u + SCAN_LOOKBACK_BITS >= p0;
				}
			}

			AlertReading_t r;
			if (gate && ALERT_DecodePayload32(assemble(words), &r) != ALERT_FMT_NONE) {
				r.polarity = polarity;
				r.bit_pos  = (uint16_t)p0;
				out[found++] = r;
				last_stop = pos;
				have_last = true;
				nwords = 0;      // a frame never overlaps the next
			}
		}
	}
	return found;
}
#undef GB

static int scan_bits(const uint8_t *buf, uint32_t nbits, uint8_t polarity, uint8_t max_gap,
                     bool invert, uint8_t min_idle_bits, AlertReading_t *out, int max_out)
{
	const uint8_t inv = invert ? 1u : 0u;

	if (polarity != ALERT_POL_ANY)
		return scan_polarity(buf, nbits, polarity, max_gap, inv, min_idle_bits, out, max_out);

	int n = scan_polarity(buf, nbits, ALERT_POL_NEGATIVE, max_gap, inv, min_idle_bits, out, max_out);
	if (n < max_out)
		n += scan_polarity(buf, nbits, ALERT_POL_STANDARD, max_gap, inv, min_idle_bits,
		                   out + n, max_out - n);
	return n;
}

int ALERT_ScanBitsEx(const uint8_t *buf, uint32_t nbits, uint8_t polarity,
                     uint8_t max_gap, bool invert, AlertReading_t *out, int max_out)
{
	return scan_bits(buf, nbits, polarity, max_gap, invert, 0, out, max_out);
}

int ALERT_ScanBitsGated(const uint8_t *buf, uint32_t nbits, uint8_t polarity, uint8_t max_gap,
                        bool invert, uint8_t min_idle_bits, AlertReading_t *out, int max_out)
{
	return scan_bits(buf, nbits, polarity, max_gap, invert, min_idle_bits, out, max_out);
}

int ALERT_ScanBits(const uint8_t *buf, uint32_t nbits, uint8_t polarity,
                   uint8_t max_gap, AlertReading_t *out, int max_out)
{
	return ALERT_ScanBitsEx(buf, nbits, polarity, max_gap, false, out, max_out);
}

/* ---------------------------------------------------------------------------
 * ALERT_ScanSamples: a software UART over the FSK engine's oversampled stream.
 *
 * tools/alert/scan_samples.py is a statement-for-statement Python port, used by
 * the host judge on logged captures; test_decode.c checks the two agree. Change
 * both together.
 *
 * Positions are in samples, Q8. Sample x is taken at time x, so a transition
 * seen between samples x-1 and x happened at x - 0.5 on average.
 *
 * Why each word is re-timed from its own transitions: the stream is k = TONE2/300
 * samples per bit, k is often fractional, and the AL200X only promises 300 baud
 * +-2%. A fixed phase grid (decimation) is half a bit out 25 bits into a 2%
 * error, inside one frame; a UART that re-aligns on every start edge drifts
 * at most one word's worth.
 * Within the word the phase and rate are fitted rather than assumed: that is
 * what keeps k = 3.47 and below decoding, where the +-floor(k/4) centre windows
 * hold a single sample and so say nothing about timing.
 */
#define SCAN_MAX_GAP    20u     // idle bits allowed between the words of one frame
#define SCAN_MAX_TRANS  20      // a word has at most 10 transitions; more is noise
#define SCAN_MED_MIN_Q8 512u    // median-of-3 only once every bit spans 2+ samples
#define SCAN_NSAMP_MAX  0xFFFFu // bit_pos is 16 bits

typedef struct {
	const uint8_t *buf;
	uint32_t       n;
	uint8_t        inv;
	uint8_t        med;
} Samples_t;

// Sample i, complemented when inverting, through a median-of-3 that removes
// single-sample glitches. Below 2 samples per bit a real bit can be one
// sample long, so the filter would eat data and is skipped.
static uint8_t samp(const Samples_t *s, uint32_t i)
{
	const uint8_t b = ALERT_GetBit(s->buf, i) ^ s->inv;
	if (!s->med || i == 0 || i + 1u >= s->n)
		return b;
	const uint8_t sum = b + (ALERT_GetBit(s->buf, i - 1u) ^ s->inv)
	                      + (ALERT_GetBit(s->buf, i + 1u) ^ s->inv);
	return sum >= 2;
}

// Candidate word timings: phase in eighths of a bit, rate in 1/40ths (2.5%).
static const int8_t scan_phase_steps[5] = { 0, -1, 1, -2, 2 };
static const int8_t scan_rate_steps[3]  = { 0, -1, 1 };

// Read the 10-bit word whose start edge is at sample e (samp(e-1) idle,
// samp(e) start). Every transition up to the stop-bit centre is scored against
// each candidate timing by its distance from the nearest predicted bit boundary
// (heavily so once it lies more than half a sample off), and the best timing
// wins; ties go to the nominal one. Each bit is then the majority over
// +-floor(spb/4) samples about its centre.
// Returns 1 with *bits (bit j = ALERT bit j, 0 start .. 9 stop) and *stop_q8
// (the stop-bit centre); 0 if the word would run past the end of the buffer;
// -1 if the span holds too many transitions to be a word.
static int read_word(const Samples_t *s, uint32_t e, uint32_t spb_q8,
                     uint16_t *bits, int32_t *stop_q8)
{
	const int32_t spb  = (int32_t)spb_q8;
	const int32_t q    = spb >> 3;
	const int32_t dr   = spb / 40;
	const int32_t h    = spb >> 10;
	const int32_t b0   = ((int32_t)e << 8) - 128;
	const int32_t cmax = (b0 + 2 * q + ((spb + dr) >> 1) + 9 * (spb + dr) + 128) >> 8;
	if (cmax + h >= (int32_t)s->n)
		return 0;

	uint8_t ts[SCAN_MAX_TRANS];   // transitions, as sample offsets from e
	uint8_t nt = 0;
	const int32_t xend = (b0 + 9 * spb + (spb >> 1)) >> 8;
	uint8_t p = samp(s, e - 1u);
	for (int32_t x = (int32_t)e; x <= xend; x++) {
		const uint8_t v = samp(s, (uint32_t)x);
		if (v != p) {
			if (nt == SCAN_MAX_TRANS)
				return -1;
			ts[nt++] = (uint8_t)(x - (int32_t)e);
		}
		p = v;
	}

	int32_t best_cost = INT32_MAX;
	int32_t best_base = 0;
	int32_t best_r    = spb;
	for (uint8_t ri = 0; ri < 3; ri++) {
		const int32_t r = spb + scan_rate_steps[ri] * dr;
		for (uint8_t pi = 0; pi < 5; pi++) {
			int32_t bnd  = b0 + scan_phase_steps[pi] * q;   // nearest predicted boundary
			int32_t mid  = bnd + (r >> 1);                  // past this the next one is nearer
			int32_t cost = 0;
			for (uint8_t k = 0; k < nt; k++) {
				const int32_t t = b0 + ((int32_t)ts[k] << 8);
				while (t > mid) {
					bnd += r;
					mid += r;
				}
				int32_t d = t - bnd;
				if (d < 0)
					d = -d;
				cost += d;
				if (d > 128)
					cost += 4 * (d - 128);
			}
			if (cost < best_cost) {
				best_cost = cost;
				best_base = b0 + scan_phase_steps[pi] * q + (r >> 1);
				best_r    = r;
			}
		}
	}

	uint16_t w = 0;
	for (int32_t j = 0; j < 10; j++) {
		const int32_t c = (best_base + j * best_r + 128) >> 8;
		int32_t ones = 0;
		for (int32_t x = c - h; x <= c + h; x++)
			ones += samp(s, (uint32_t)x);
		if (2 * ones > 2 * h + 1)
			w |= (uint16_t)(1u << j);
	}
	*bits    = w;
	*stop_q8 = best_base + 9 * best_r;
	return 1;
}

static int scan_samples_pol(const Samples_t *s, uint32_t spb_q8, uint8_t polarity,
                            uint8_t min_idle_bits, AlertReading_t *out, int max_out)
{
	const uint8_t  idle  = (polarity == ALERT_POL_NEGATIVE) ? 0u : 1u;
	const uint8_t  start = idle ^ 1u;
	const uint32_t need  = ((uint32_t)min_idle_bits * spb_q8) >> 8;

	uint8_t  words[4];
	uint32_t edge[4];
	uint8_t  nwords       = 0;
	int      found        = 0;
	int32_t  last_stop_q8 = -1;   // end of the last accepted frame's final stop bit

	if (s->n < 2u)
		return 0;
	uint32_t i    = 1;
	uint8_t  prev = samp(s, 0);
	while (i < s->n && found < max_out) {
		const uint8_t cur = samp(s, i);
		if (prev != idle || cur != start) {
			prev = cur;
			i++;
			continue;
		}
		const uint32_t e = i;
		uint16_t bits    = 0;
		int32_t  stop_q8 = 0;
		const int rc = read_word(s, e, spb_q8, &bits, &stop_q8);
		if (rc == 0)
			break;                 // no whole word fits past here
		if (rc < 0 || (uint8_t)(bits & 1u) != start || (uint8_t)((bits >> 9) & 1u) != idle) {
			nwords = 0;            // framing error: slip one sample
			prev = cur;
			i++;
			continue;
		}

		// drop the run if this word does not follow the previous one closely
		if (nwords && ((e - edge[nwords - 1]) << 8) > (10u + SCAN_MAX_GAP) * spb_q8)
			nwords = 0;
		if (nwords == 4) {
			words[0] = words[1]; words[1] = words[2]; words[2] = words[3];
			edge[0]  = edge[1];  edge[1]  = edge[2];  edge[2]  = edge[3];
			nwords = 3;
		}
		words[nwords] = (uint8_t)(bits >> 1);   // data bits LSB first, as they lie
		edge[nwords]  = e;
		nwords++;

		// hunt for the next start from this word's stop-bit centre
		const uint32_t c = (uint32_t)((stop_q8 + 128) >> 8);
		prev = samp(s, c);
		i    = c + 1u;

		if (nwords == 4) {
			const uint32_t e0 = edge[0];
			bool gate = last_stop_q8 >= 0 &&
			            (e0 << 8) <= (uint32_t)last_stop_q8 + SCAN_EXEMPT_BITS * spb_q8;
			if (!gate && e0 >= need) {
				uint32_t j = e0 - need;
				while (j < e0 && samp(s, j) == idle)
					j++;
				gate = (j == e0);
			}

			AlertReading_t r;
			if (gate && ALERT_DecodePayload32(assemble(words), &r) != ALERT_FMT_NONE) {
				r.polarity = polarity;
				r.bit_pos  = (uint16_t)e0;
				out[found++] = r;
				last_stop_q8 = stop_q8 + (int32_t)(spb_q8 >> 1);
				nwords = 0;
			}
		}
	}
	return found;
}

int ALERT_ScanSamples(const uint8_t *buf, uint32_t nsamp, uint32_t spb_q8, uint8_t polarity,
                      bool invert, uint8_t min_idle_bits, AlertReading_t *out, int max_out)
{
	if (spb_q8 < ALERT_SPB_Q8_MIN || spb_q8 > ALERT_SPB_Q8_MAX || max_out <= 0)
		return 0;

	Samples_t s;
	s.buf = buf;
	s.n   = nsamp > SCAN_NSAMP_MAX ? SCAN_NSAMP_MAX : nsamp;
	s.inv = invert ? 1u : 0u;
	s.med = spb_q8 >= SCAN_MED_MIN_Q8 ? 1u : 0u;

	if (polarity != ALERT_POL_ANY)
		return scan_samples_pol(&s, spb_q8, polarity, min_idle_bits, out, max_out);

	int n = scan_samples_pol(&s, spb_q8, ALERT_POL_NEGATIVE, min_idle_bits, out, max_out);
	if (n < max_out)
		n += scan_samples_pol(&s, spb_q8, ALERT_POL_STANDARD, min_idle_bits,
		                      out + n, max_out - n);
	return n;
}
