/* Host unit test for app/alert_decode.c
 *
 *   make -C test        (or: gcc -I.. test_alert_decode.c ../app/alert_decode.c -o test_alert_decode && ./test_alert_decode)
 *
 * Vectors come from MegaNet packets.js: its built-in EXAMPLE
 * 1000001110111010101011111100001111111100 decodes as EIF, A=2784, D=1599,
 * FCS=62, negative-logic framing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/alert_decode.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// write a bit string ("0101...") into buf starting at bit offset `at`
static void put_bits(uint8_t *buf, uint32_t at, const char *s)
{
	for (; *s; s++, at++) {
		if (*s == '1') buf[at >> 3] |=  (uint8_t)(0x80u >> (at & 7));
		else           buf[at >> 3] &= (uint8_t)~(0x80u >> (at & 7));
	}
}

// build a 40-bit ABF frame string for id/value in the given polarity
static void abf_frame(uint16_t a, uint16_t d, int negative, char out[41])
{
	int p[32]; int n = 0;
	for (int i = 0; i < 6; i++) { p[n++] = (a >> i) & 1; }
	p[n++] = 1; p[n++] = 0;
	for (int i = 6; i < 12; i++) { p[n++] = (a >> i) & 1; }
	p[n++] = 1; p[n++] = 0;
	p[n++] = (a >> 12) & 1;
	for (int i = 0; i < 5; i++) { p[n++] = (d >> i) & 1; }
	p[n++] = 1; p[n++] = 1;
	for (int i = 5; i < 11; i++) { p[n++] = (d >> i) & 1; }
	p[n++] = 1; p[n++] = 1;
	int o = 0;
	for (int k = 0; k < 4; k++) {
		out[o++] = negative ? '1' : '0';
		for (int i = 0; i < 8; i++) out[o++] = p[8 * k + i] ? '1' : '0';
		out[o++] = negative ? '0' : '1';
	}
	out[o] = 0;
}

static uint32_t payload_from_string(const char *bits32)
{
	uint32_t v = 0;
	for (int i = 0; i < 32; i++) v = (v << 1) | (bits32[i] == '1');
	return v;
}

int main(void)
{
	AlertReading_t r[8];

	// --- CRC-6 against MegaNet's example: A=2784, D=1599 -> 62
	CHECK(ALERT_Crc6(((uint32_t)2784 << 11) | 1599, 24) == 62, "crc6 example");

	// --- payload decode: MegaNet EXAMPLE payload 0x07D5F8FE is EIF 2784 / 1599
	memset(r, 0, sizeof(r));
	CHECK(ALERT_DecodePayload32(0x07D5F8FEu, &r[0]) == ALERT_FMT_EIF, "EIF format");
	CHECK(r[0].id == 2784 && r[0].value == 1599, "EIF id/value %u/%u", r[0].id, r[0].value);

	// corrupt one FCS bit -> must fail
	CHECK(ALERT_DecodePayload32(0x07D5F8FFu, &r[0]) == ALERT_FMT_NONE, "EIF bad crc rejected");

	// --- ABF payload built by the same rules as packets.js encodeFormat
	char f[41];
	abf_frame(6129, 1599, 1, f);
	{
		char bits32[33]; int o = 0;
		for (int k = 0; k < 4; k++) for (int i = 1; i <= 8; i++) bits32[o++] = f[10 * k + i];
		bits32[32] = 0;
		uint32_t pl = payload_from_string(bits32);
		CHECK(pl == 0x8EFAFF8Fu, "ABF payload 0x%08X", (unsigned)pl);
		CHECK(ALERT_DecodePayload32(pl, &r[0]) == ALERT_FMT_ABF, "ABF format");
		CHECK(r[0].id == 6129 && r[0].value == 1599, "ABF id/value %u/%u", r[0].id, r[0].value);
	}

	// --- A2C record: 6129 / 1599 -> bytes F1, C7, 3F, 00
	{
		const uint8_t b[4] = { 6129 & 0xff, (uint8_t)(((1599 >> 8) << 5) | (6129 >> 8)), 1599 & 0xff, 0 };
		CHECK(ALERT_DecodeA2C(b, &r[0]) && r[0].id == 6129 && r[0].value == 1599, "A2C decode");
		const uint8_t bad[4] = { b[0], b[1], b[2], 1 };
		CHECK(!ALERT_DecodeA2C(bad, &r[0]), "A2C status != 0 rejected");
	}

	// --- bitstream scan: MegaNet EXAMPLE frame (negative logic) at an odd offset in idle-low fill
	{
		uint8_t buf[64]; memset(buf, 0x00, sizeof(buf));
		put_bits(buf, 37, "1000001110111010101011111100001111111100");
		int n = ALERT_ScanBits(buf, sizeof(buf) * 8, ALERT_POL_ANY, 20, r, 8);
		CHECK(n == 1, "scan EXAMPLE found %d", n);
		CHECK(n >= 1 && r[0].format == ALERT_FMT_EIF && r[0].id == 2784 && r[0].value == 1599
		      && r[0].polarity == ALERT_POL_NEGATIVE && r[0].bit_pos == 37,
		      "scan EXAMPLE fields fmt=%u id=%u val=%u pol=%u pos=%u",
		      r[0].format, r[0].id, r[0].value, r[0].polarity, r[0].bit_pos);
	}

	// --- two ABF frames, standard polarity, separated by idle, in random noise
	{
		uint8_t buf[128];
		srand(1234);
		for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)rand();
		char f1[41], f2[41];
		abf_frame(700, 42, 0, f1);
		abf_frame(8191, 2047, 0, f2);
		// idle-high run, frame, 7 idle bits, frame, idle
		put_bits(buf, 200, "1111111111111111");
		put_bits(buf, 216, f1);
		put_bits(buf, 256, "1111111");
		put_bits(buf, 263, f2);
		put_bits(buf, 303, "11111111");
		int n = ALERT_ScanBits(buf, sizeof(buf) * 8, ALERT_POL_STANDARD, 20, r, 8);
		int hit1 = 0, hit2 = 0, extra = 0;
		for (int i = 0; i < n; i++) {
			if (r[i].id == 700  && r[i].value == 42   && r[i].format == ALERT_FMT_ABF && r[i].bit_pos == 216) hit1++;
			else if (r[i].id == 8191 && r[i].value == 2047 && r[i].format == ALERT_FMT_ABF && r[i].bit_pos == 263) hit2++;
			else extra++;
		}
		CHECK(hit1 == 1 && hit2 == 1, "scan two ABF frames in noise: n=%d hit1=%d hit2=%d", n, hit1, hit2);
		CHECK(extra == 0, "no false frames from noise (extra=%d)", extra);
	}

	// --- pure random noise. The BoM spec notes ABF/EIF only carry 8 fixed bits per
	// 32-bit payload, so 1 random word-aligned frame in 256 passes; the UART
	// emulation (start after idle, stop bit) adds ~5 bits. The app further gates
	// on squelch/RSSI and the station table. Guard the rate here so a regression
	// in the framing checks is caught.
	{
		uint8_t buf[256];
		srand(99);
		int total = 0;
		for (int trial = 0; trial < 50; trial++) {
			for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)rand();
			total += ALERT_ScanBits(buf, sizeof(buf) * 8, ALERT_POL_ANY, 20, r, 8);
		}
		CHECK(total <= 25, "noise false positives = %d in 100 kbit (format allows ~1/256 per random word)", total);
	}

	// --- max_gap: frames whose words are too far apart are not joined
	{
		uint8_t buf[64]; memset(buf, 0x00, sizeof(buf));
		char f1[41]; abf_frame(1234, 5, 1, f1);
		char w[11]; memcpy(w, f1, 10); w[10] = 0;
		put_bits(buf, 8,  w);                 // word 1 alone
		put_bits(buf, 60, f1 + 10);           // words 2..4 after a 42-bit gap
		int n = ALERT_ScanBits(buf, sizeof(buf) * 8, ALERT_POL_NEGATIVE, 20, r, 8);
		CHECK(n == 0, "gap-separated words not joined (n=%d)", n);
		n = ALERT_ScanBits(buf, sizeof(buf) * 8, ALERT_POL_NEGATIVE, 50, r, 8);
		CHECK(n == 1 && r[0].id == 1234, "gap tolerated when max_gap allows (n=%d)", n);
	}

	printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
	return fails ? 1 : 0;
}
