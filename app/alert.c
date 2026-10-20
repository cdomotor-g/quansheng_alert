/* ALERT telemetry receiver app - see alert.h for the overview.
 *
 * Copyright 2026 cdomotor-g. Apache-2.0, like the egzumer base it lives in.
 */
#ifdef ENABLE_ALERT

#include <string.h>

#include "app/alert.h"
#include "app/alert_decode.h"
#include "app/alert_stations.h"
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"
#ifdef ENABLE_UART
	#include "driver/uart.h"
#endif
#include "ARMCM0.h"
#include "board.h"
#include "bsp/dp32g030/portcon.h"
#include "bsp/dp32g030/saradc.h"
#include "bsp/dp32g030/syscon.h"
#include "driver/adc.h"
#include "app/alert_demod.h"

// ---------------------------------------------------------------------------
// configuration (persisted in gEeprom.ALERT_CFG[5], see settings.c)

static struct {
	bool voice;        // read new readings out loud
	bool gate;         // SQ GATE: a burst must peak well clear of the noise floor
	bool show_unknown; // show addresses that are not in the station table
	bool monitor;      // speaker on while the squelch is open: hear the bursts
	bool confirm;      // require the same reading twice in one burst
} cfg;

// 0xA0 marked the layout of the BK4819-modem era, whose defaults could not
// decode; a new magic puts every radio on the defaults below once.
#define CFG_MAGIC 0xB0u

void ALERT_LoadConfig(void)
{
	const uint8_t *b = gEeprom.ALERT_CFG;
	if ((b[4] & 0xF0u) != CFG_MAGIC) {
		cfg.voice = true; cfg.gate = true; cfg.show_unknown = true;
		cfg.monitor = false; cfg.confirm = false;
		return;
	}
	cfg.voice        = b[0] & (1u << 0);
	cfg.gate         = b[0] & (1u << 1);
	cfg.show_unknown = b[0] & (1u << 2);
	cfg.monitor      = b[0] & (1u << 3);
	cfg.confirm      = b[0] & (1u << 4);
}

void ALERT_StoreConfig(void)
{
	uint8_t *b = gEeprom.ALERT_CFG;
	b[0] = (uint8_t)(cfg.voice | (cfg.gate << 1) | (cfg.show_unknown << 2) | (cfg.monitor << 3) | (cfg.confirm << 4));
	b[1] = b[2] = b[3] = 0;
	b[4] = CFG_MAGIC;
}

// ---------------------------------------------------------------------------
// state

typedef struct {
	uint16_t id;
	uint16_t value;
	uint8_t  kind;
	uint8_t  format;
	int8_t   rssi_dbm;
	uint8_t  seq;        // burst sequence number (readings from one burst share it)
} History_t;

#define HISTORY_N 6
static History_t history[HISTORY_N];
static uint8_t   historyCount;
static uint8_t   burstSeq;

static struct {
	uint16_t bursts;     // squelch openings that qualified as a burst
	uint16_t frames;     // valid frames decoded
	uint16_t gated;      // squelch openings that did not qualify
	uint16_t unknown;    // frames whose address is not in the table
	uint16_t inv;        // frames only the complemented data sense decoded
} stats;

static bool     sqOpen;
static int16_t  rssiDbm;
static bool     running;
static bool     redraw;
static uint8_t  view;            // VIEW_*
static uint8_t  setIndex;        // selected settings row
static uint8_t  lastCap[8];      // first bytes of the last burst, for the raw view
static uint16_t lastCapLen;
static uint16_t lastLevel[3];    // ADC min / max / mean over the last burst
static int16_t  lastPeak;        // peak RSSI of the last burst
static uint16_t tick;            // 10 ms timeslices
static bool     voiceBusy;

enum { VIEW_MAIN = 0, VIEW_SETTINGS, VIEW_RAW };

// ---------------------------------------------------------------------------
// MCU ADC sampler: PA8 (ADC CH3) at 9600 Hz while a burst is open
//
// PA8 is also UART1 RX, the programming cable's receive line, so it is an ADC
// input only while the app runs; on exit it goes back. The 9600 Hz SysTick runs
// only while the squelch is open, as in quansheng_alert_v3: the sampler's own
// interference is why fagci's spectrum code dropped the same arrangement.

volatile bool gAlertAdcRun;
static bool   sampling;

static void AdcInit(void)
{
	ADC_Config_t c;
	memset(&c, 0, sizeof(c));
	c.CLK_SEL            = SYSCON_CLK_SEL_W_SARADC_SMPL_VALUE_DIV2;
	c.CH_SEL             = ADC_CH3;
	c.AVG                = SARADC_CFG_AVG_VALUE_1_SAMPLE;
	c.CONT               = SARADC_CFG_CONT_VALUE_SINGLE;
	c.MEM_MODE           = SARADC_CFG_MEM_MODE_VALUE_CHANNEL;
	c.SMPL_CLK           = SARADC_CFG_SMPL_CLK_VALUE_INTERNAL;
	c.SMPL_WIN           = SARADC_CFG_SMPL_WIN_VALUE_15_CYCLE;
	c.SMPL_SETUP         = SARADC_CFG_SMPL_SETUP_VALUE_1_CYCLE;
	c.ADC_TRIG           = SARADC_CFG_ADC_TRIG_VALUE_CPU;
	c.CALIB_KD_VALID     = SARADC_CALIB_KD_VALID_VALUE_YES;
	c.CALIB_OFFSET_VALID = SARADC_CALIB_OFFSET_VALID_VALUE_YES;
	c.DMA_EN             = SARADC_CFG_DMA_EN_VALUE_DISABLE;
	c.IE_CHx_EOC         = SARADC_IE_CHx_EOC_VALUE_NONE;
	c.IE_FIFO_FULL       = SARADC_IE_FIFO_FULL_VALUE_DISABLE;
	c.IE_FIFO_HFULL      = SARADC_IE_FIFO_HFULL_VALUE_DISABLE;

	// PA8 (pin 9): UART1 RX -> SAR ADC channel 3
	PORTCON_PORTA_SEL1 = (PORTCON_PORTA_SEL1 & ~PORTCON_PORTA_SEL1_A8_MASK) | PORTCON_PORTA_SEL1_A8_BITS_SARADC_CH3;

	ADC_Configure(&c);
	ADC_Enable();
	ADC_SoftReset();
}

static void SamplerStart(void)
{
	ALERT_DemodReset();
	ADC_Start();                             // so the first tick finds a conversion done
	gAlertAdcRun = true;
	SysTick_Config(48000000u / ALERT_DEMOD_FS);
	sampling = true;
}

static void SamplerStop(void)
{
	if (!sampling)
		return;
	gAlertAdcRun = false;                    // no tick touches the demodulator after this
	SysTick_Config(480000);                  // back to the 10 ms system tick
	sampling = false;
}

static void AdcDeinit(void)
{
	SamplerStop();
	ADC_Disable();
	PORTCON_PORTA_SEL1 = (PORTCON_PORTA_SEL1 & ~PORTCON_PORTA_SEL1_A8_MASK) | PORTCON_PORTA_SEL1_A8_BITS_UART1_RX;
	BOARD_ADC_Init();                        // battery channels back
}

// 9600 Hz, from SystickHandler()
void ALERT_AdcTick(void)
{
	const int32_t raw = ADC_GetValue(ADC_CH3);
	ADC_Start();
	ALERT_DemodSample(raw);
}

// The radio's own receive path, with the AF held at FM. EARO, which the mod
// taps, carries audio only while the AF is selected: the earlier code left it
// at MUTE unless MONITOR was on, so the ADC sampled silence. The speaker has
// its own switch, the audio path, which MONITOR opens while the squelch is
// open - the bursts, not the hiss between them - and the voice while it talks.
static void AudioRoute(void)
{
	BK4819_SetAF(BK4819_AF_FM);
	if ((cfg.monitor && sqOpen) || voiceBusy) AUDIO_AudioPathOn();
	else                                      AUDIO_AudioPathOff();
}

// ---------------------------------------------------------------------------
// decoded readings

#ifdef ENABLE_VOICE
static void SpeakDigits(uint16_t id, uint16_t value)
{
	// station id then value, digit by digit (the voice ROM has digits only)
	uint8_t n = 0;
	uint16_t v[2] = { id, value };
	gVoiceWriteIndex = 0;
	gVoiceReadIndex  = 0;
	for (uint8_t k = 0; k < 2; k++) {
		uint8_t d[5], nd = 0;
		do { d[nd++] = v[k] % 10u; v[k] /= 10u; } while (v[k]);
		while (nd && n < 8)
			gVoiceID[n++] = (VOICE_ID_t)d[--nd];
	}
	gVoiceWriteIndex = n;
	AUDIO_PlaySingleVoice(false);
	voiceBusy = true;
}
#endif

static void Announce(const History_t *h)
{
	BACKLIGHT_TurnOn();
#ifdef ENABLE_VOICE
	if (cfg.voice && gEeprom.VOICE_PROMPT != VOICE_PROMPT_OFF) {
		SpeakDigits(h->id, h->value);
		return;
	}
#endif
	(void)h;
}

static void PushHistory(const AlertReading_t *r, int16_t rssi, uint8_t kind, uint8_t seq)
{
	if (historyCount < HISTORY_N)
		historyCount++;
	for (uint8_t i = historyCount - 1; i > 0; i--)
		history[i] = history[i - 1];
	history[0].id       = r->id;
	history[0].value    = r->value;
	history[0].kind     = kind;
	history[0].format   = r->format;
	history[0].rssi_dbm = (int8_t)((rssi < -127) ? -127 : (rssi > 0 ? 0 : rssi));
	history[0].seq      = seq;
}

// One burst's decoded frames: de-duplicated, optionally confirmed, shown,
// logged to the UART and announced. Unknown addresses are counted whatever
// UNKNOWN says; it only decides whether they are shown.
static void Deliver(const AlertReading_t *r, int n, int16_t rssi)
{
	uint8_t announced = 0;

	burstSeq++;
	for (int i = 0; i < n; i++) {
		// collapse repeats of the same reading inside one burst
		bool dup = false;
		for (int j = 0; j < i; j++)
			if (r[j].id == r[i].id && r[j].value == r[i].value) { dup = true; break; }
		if (dup)
			continue;
		if (cfg.confirm) {
			bool again = false;
			for (int j = i + 1; j < n; j++)
				if (r[j].id == r[i].id && r[j].value == r[i].value) { again = true; break; }
			if (!again)
				continue;
		}

		const char *name; uint8_t kind;
		const bool known = ALERT_LookupStation(r[i].id, &name, &kind);
		if (!known) {
			stats.unknown++;
			if (!cfg.show_unknown)
				continue;
		}
		stats.frames++;
		PushHistory(&r[i], rssi, kind, burstSeq);

#ifdef ENABLE_UART
		{
			char line[48];
			sprintf(line, "ALERT,%u,%u,%s,%d,%s\r\n", r[i].id, r[i].value,
			        r[i].format == ALERT_FMT_EIF ? "EIF" : "ABF", rssi, name);
			UART_Send(line, strlen(line));
		}
#endif
		if (!announced++)
			Announce(&history[0]);
	}
}

// ---------------------------------------------------------------------------
// bursts, as quansheng_alert_v3 finds and judges them
//
// The squelch opening starts the sampler and its closing, DRAIN_TICKS later,
// ends it: a flicker that re-opens inside that is the same burst. A burst
// qualifies on its length and, with SQ GATE on, on rising well clear of a
// slowly tracked noise floor - at SQL 1.0 the squelch opens on noise too, and
// ABF's 8 check bits pass on noise often enough to matter.

#define DRAIN_TICKS       4u        // 40 ms
#define BURST_MIN_TICKS   10u       // 100 ms: shorter is a flicker
#define BURST_MAX_TICKS   150u      // 1.5 s: longer is not an ALERT burst
#define BURST_MARGIN_DB   15
#define FLOOR_RISE_TICKS  500u      // the floor drifts back up 1 dB per 5 s

static bool     sqPrev;
static bool     draining;
static bool     tainted;            // the loop blocked while the burst was open
static uint16_t openTick, lostTick;
static int16_t  rssiFloor;          // slow minimum, dBm; 0 until the first reading
static int16_t  burstPeak;

static void BurstOpen(void)
{
	openTick  = tick;
	burstPeak = rssiDbm;
	tainted   = false;
	SamplerStart();
	AudioRoute();
	redraw = true;
}

static void BurstFinish(void)
{
	AlertReading_t r[8];
	const uint16_t dur = (uint16_t)(lostTick - openTick);

	SamplerStop();
	draining = false;
	AudioRoute();
	redraw   = true;

	if (tainted || dur < BURST_MIN_TICKS || dur > BURST_MAX_TICKS ||
	    (cfg.gate && burstPeak < (int16_t)(rssiFloor + BURST_MARGIN_DB))) {
		stats.gated++;
		return;
	}
	stats.bursts++;
	lastPeak = burstPeak;

	const uint16_t n = ALERT_DemodBitCount();
	const uint8_t *bits = ALERT_DemodBits();
	lastCapLen = n;
	memcpy(lastCap, bits, sizeof(lastCap));
	ALERT_DemodLevels(&lastLevel[0], &lastLevel[1], &lastLevel[2]);
	if (n < 40u)
		return;

	// Both data senses; POL_ANY covers both framings. Gated: sampling starts
	// at the squelch open, inside the preamble, so a real frame always has its
	// 12 idle bits ahead of it, and noise rarely does.
	int na = ALERT_ScanBitsGated(bits, n, ALERT_POL_ANY, 20, false, 12u, r, 8);
	const int na0 = na;
	if (na < 8)
		na += ALERT_ScanBitsGated(bits, n, ALERT_POL_ANY, 20, true, 12u, r + na, 8 - na);
	stats.inv += (uint16_t)(na - na0);
	Deliver(r, na, burstPeak);
}

static void RxPoll(void)
{
	// squelch state straight from the chip: REG_0C<1> 1 = open
	sqOpen = (BK4819_ReadRegister(BK4819_REG_0C) & 2u) != 0;
	if (sqOpen && !sqPrev) {
		if (draining) draining = false;      // a flicker inside one burst
		else          BurstOpen();
	} else if (!sqOpen && sqPrev) {
		draining = true;
		lostTick = tick;
	}
	sqPrev = sqOpen;

	if (draining && (uint16_t)(tick - lostTick) >= DRAIN_TICKS)
		BurstFinish();
	// A squelch held open well past any burst (a voice on the channel) is not
	// sampled for its whole length; it will not qualify when it closes.
	else if (sampling && sqOpen && (uint16_t)(tick - openTick) > BURST_MAX_TICKS + 50u)
		SamplerStop();

	// keep REG_02 clear; bounded, so a flag that will not clear cannot hang the loop
	for (uint8_t guard = 0; guard < 16 && (BK4819_ReadRegister(BK4819_REG_0C) & 1u); guard++)
		BK4819_WriteRegister(BK4819_REG_02, 0);
}

// Every 10 ms: the RSSI, the burst's peak and the noise floor.
static void RssiTick(void)
{
	rssiDbm = BK4819_GetRSSI_dBm();
	if (sqOpen && rssiDbm > burstPeak)
		burstPeak = rssiDbm;
	if (rssiFloor == 0 || rssiDbm < rssiFloor)
		rssiFloor = rssiDbm;
	if ((tick % FLOOR_RISE_TICKS) == 0 && rssiFloor < 0)
		rssiFloor++;
}

// ---------------------------------------------------------------------------
// display

static void FormatValue(char *out, uint16_t value, uint8_t kind)
{
	if (value == ALERT_VALUE_FULL_SCALE) {
		strcpy(out, "FULL");
		return;
	}
	if (kind == ALERT_KIND_BATT)   // tenths of a volt (MegaNet convention)
		sprintf(out, "%u.%uV", value / 10u, value % 10u);
	else                           // rain: tip count; level: raw, scaled per site
		sprintf(out, "%u", value);
}

static void DrawStatus(void)
{
	char s[24];
	memset(gStatusLine, 0, sizeof(gStatusLine));
	UI_PrintStringSmallBufferNormal("ALERT", gStatusLine + 0);
	sprintf(s, "%4ddBm %s", rssiDbm, sqOpen ? "SQ" : "  ");
	UI_PrintStringSmallBufferNormal(s, gStatusLine + 60);
	if (sampling)
		UI_PrintStringSmallBufferNormal("*", gStatusLine + 122);
	ST7565_BlitStatusLine();
}

static void DrawMain(void)
{
	char s[32];

	UI_DisplayClear();

	if (historyCount == 0) {
		const uint32_t f = gRxVfo->pRX->Frequency;
		sprintf(s, "%u.%05u MHz", (unsigned)(f / 100000u), (unsigned)(f % 100000u));
		UI_PrintStringSmallNormal(s, 0, 127, 0);
		UI_PrintStringSmallNormal("WAITING FOR ALERT", 0, 127, 2);
		sprintf(s, "%u SITES %s", ALERT_StationCount(), ALERT_StationSource());
		UI_PrintStringSmallNormal(s, 0, 127, 3);
		sprintf(s, "BURSTS %u  NF %d", stats.bursts, rssiFloor);
		UI_PrintStringSmallNormal(s, 0, 127, 4);
		sprintf(s, "SQL %u.%u  %s", gEeprom.SQUELCH_LEVEL, gEeprom.SQUELCH_TENTHS,
		        cfg.voice ? "VOICE" : "QUIET");
		UI_PrintStringSmallNormal(s, 0, 127, 5);
		UI_PrintStringSmallNormal("MENU=SET *=VOICE 1=RAW", 0, 127, 6);
	} else {
		const History_t *h = &history[0];
		const char *name; uint8_t kind;
		ALERT_LookupStation(h->id, &name, &kind);

		// line 0: station name (or unknown), line 1-2: value big, right: kind/id
		if (name[0]) {
			UI_PrintStringSmallNormal(name, 0, 0, 0);
		} else {
			sprintf(s, "ID %u NOT IN TABLE", h->id);
			UI_PrintStringSmallNormal(s, 0, 0, 0);
		}
		FormatValue(s, h->value, h->kind);
		UI_PrintString(s, 0, 0, 1, 8);
		sprintf(s, "%s", ALERT_KindLabel(h->kind));
		UI_PrintStringSmallNormal(s, 88, 0, 1);
		sprintf(s, "#%u", h->id);
		UI_PrintStringSmallNormal(s, 88, 0, 2);

		// lines 3..6: history
		for (uint8_t i = 0; i < 4 && i < historyCount; i++) {
			const History_t *e = &history[i];
			const char *n2; uint8_t k2; char v[8];
			ALERT_LookupStation(e->id, &n2, &k2);
			FormatValue(v, e->value, e->kind);
			sprintf(s, "%4u %-9.9s %6s", e->id, n2[0] ? n2 : "?", v);
			UI_PrintStringSmallNormal(s, 0, 0, 3 + i);
		}
	}
	ST7565_BlitFullScreen();
}

// settings rows
enum { SET_VOICE, SET_GATE, SET_UNKNOWN, SET_CONFIRM, SET_MONITOR, SET_SQL, SET_N };

static const char *const setNames[SET_N] = {
	"VOICE", "SQ GATE", "UNKNOWN", "CONFIRM", "MONITOR", "SQL LEVEL"
};

static void SetValueString(uint8_t idx, char *s)
{
	static const char *const onoff[2] = { "OFF", "ON" };
	switch (idx) {
		case SET_VOICE:   strcpy(s, onoff[cfg.voice]); break;
		case SET_GATE:    strcpy(s, onoff[cfg.gate]); break;
		case SET_UNKNOWN: strcpy(s, cfg.show_unknown ? "SHOW" : "HIDE"); break;
		case SET_CONFIRM: strcpy(s, cfg.confirm ? "2 COPIES" : "OFF"); break;
		case SET_MONITOR: strcpy(s, onoff[cfg.monitor]); break;
		case SET_SQL:     sprintf(s, "%u.%u", gEeprom.SQUELCH_LEVEL, gEeprom.SQUELCH_TENTHS); break;
		default: s[0] = 0; break;
	}
}

static void ApplySquelch(void)
{
	RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
	BK4819_SetupSquelch(
		gRxVfo->SquelchOpenRSSIThresh,    gRxVfo->SquelchCloseRSSIThresh,
		gRxVfo->SquelchOpenNoiseThresh,   gRxVfo->SquelchCloseNoiseThresh,
		gRxVfo->SquelchCloseGlitchThresh, gRxVfo->SquelchOpenGlitchThresh);
	AudioRoute();                        // BK4819_SetupSquelch mutes the AF
}

static void StepSquelch(int dir)
{
	int lvl = gEeprom.SQUELCH_LEVEL * 10 + gEeprom.SQUELCH_TENTHS + dir;
	if (lvl < 0) lvl = 0;
	if (lvl > 90) lvl = 90;
	gEeprom.SQUELCH_LEVEL  = (uint8_t)(lvl / 10);
	gEeprom.SQUELCH_TENTHS = (uint8_t)(lvl % 10);
	ApplySquelch();
}

static void ChangeSetting(uint8_t idx, int dir)
{
	switch (idx) {
		case SET_VOICE:   cfg.voice = !cfg.voice; break;
		case SET_GATE:    cfg.gate = !cfg.gate; break;
		case SET_UNKNOWN: cfg.show_unknown = !cfg.show_unknown; break;
		case SET_CONFIRM: cfg.confirm = !cfg.confirm; break;
		case SET_MONITOR: cfg.monitor = !cfg.monitor; AudioRoute(); break;
		case SET_SQL:     StepSquelch(dir); break;
		default: break;
	}
}

static void DrawSettings(void)
{
	char s[24], v[16];
	UI_DisplayClear();
	UI_PrintStringSmallNormal("ALERT SETTINGS", 0, 127, 0);
	for (uint8_t i = 0; i < SET_N; i++) {
		SetValueString(i, v);
		sprintf(s, "%c%-9s %s", i == setIndex ? '>' : ' ', setNames[i], v);
		UI_PrintStringSmallNormal(s, 0, 0, 1 + i);
	}
	ST7565_BlitFullScreen();
}

// What a field test needs to see: whether bursts arrive and qualify, whether
// they decode and in which data sense, and what the ADC pin is doing. A pin
// with no audio shows a few counts of spread; one stuck at 0 or 4095 is
// clipping (see README, "the hardware mod").
static void DrawRaw(void)
{
	char s[28];
	UI_DisplayClear();
	UI_PrintStringSmallNormal("RAW", 0, 127, 0);
	sprintf(s, "BURST %u  DEC %u", stats.bursts, stats.frames);
	UI_PrintStringSmallNormal(s, 0, 0, 1);
	sprintf(s, "GATED %u INV %u UNK %u", stats.gated, stats.inv, stats.unknown);
	UI_PrintStringSmallNormal(s, 0, 0, 2);
	sprintf(s, "PEAK %d  NF %d", lastPeak, rssiFloor);
	UI_PrintStringSmallNormal(s, 0, 0, 3);
	sprintf(s, "ADC %u-%u M%u", lastLevel[0], lastLevel[1], lastLevel[2]);
	UI_PrintStringSmallNormal(s, 0, 0, 4);
	sprintf(s, "LAST %u BITS", lastCapLen);
	UI_PrintStringSmallNormal(s, 0, 0, 5);
	{	// the first 20 bits, so the preamble and start bits can be eyeballed
		uint8_t i;
		for (i = 0; i < 20; i++)
			s[i] = ALERT_GetBit(lastCap, i) ? '1' : '0';
		s[i] = 0;
		UI_PrintStringSmallNormal(s, 0, 0, 6);
	}
	ST7565_BlitFullScreen();
}

static void Draw(void)
{
	DrawStatus();
	switch (view) {
		case VIEW_SETTINGS: DrawSettings(); break;
		case VIEW_RAW:      DrawRaw(); break;
		default:            DrawMain(); break;
	}
	redraw = false;
}

// ---------------------------------------------------------------------------
// keys

static void OnKey(KEY_Code_t key)
{
	BACKLIGHT_TurnOn();
	redraw = true;

	if (view == VIEW_SETTINGS) {
		switch (key) {
			case KEY_UP:   ChangeSetting(setIndex, +1); break;
			case KEY_DOWN: ChangeSetting(setIndex, -1); break;
			case KEY_MENU: setIndex = (uint8_t)((setIndex + 1) % SET_N); break;
			case KEY_STAR: setIndex = (uint8_t)((setIndex + SET_N - 1) % SET_N); break;
			case KEY_EXIT:
				ALERT_StoreConfig();
				SETTINGS_SaveSettings();         // an EEPROM write: tens of ms
				if (sampling || draining)
					tainted = true;
				view = VIEW_MAIN;
				break;
			default: break;
		}
		return;
	}

	switch (key) {
		case KEY_EXIT:
			if (view == VIEW_RAW) view = VIEW_MAIN;
			else running = false;
			break;
		case KEY_MENU: view = VIEW_SETTINGS; setIndex = 0; break;
		case KEY_1:    view = (view == VIEW_RAW) ? VIEW_MAIN : VIEW_RAW; break;
		case KEY_STAR: cfg.voice = !cfg.voice; ALERT_StoreConfig(); break;
		case KEY_F:    ChangeSetting(SET_MONITOR, 0); break;
		case KEY_UP:   StepSquelch(+1); break;
		case KEY_DOWN: StepSquelch(-1); break;
		case KEY_0:
			historyCount = 0;
			memset(&stats, 0, sizeof(stats));
			break;
		case KEY_5:
			if (historyCount) Announce(&history[0]);
			break;
		default: break;
	}
}

// ---------------------------------------------------------------------------
// main loop

void APP_RunAlert(void)
{
	KEY_Code_t lastKey = KEY_INVALID;

	ALERT_LoadConfig();
	running = true;
	view    = VIEW_MAIN;
	redraw  = true;
	memset(&stats, 0, sizeof(stats));
	historyCount = 0;
	sqPrev = draining = false;
	rssiFloor = 0;
	lastPeak  = 0;

	// normal RX on the current VFO, then squelch as configured
	RADIO_SetupRegisters(true);
	FUNCTION_Select(FUNCTION_FOREGROUND);
	rssiDbm = BK4819_GetRSSI_dBm();
	AdcInit();
	AudioRoute();
	// squelch edges are read from REG_0C; REG_02 is kept clear in RxPoll
	BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_SQUELCH_FOUND | BK4819_REG_3F_SQUELCH_LOST);
	BK4819_WriteRegister(BK4819_REG_02, 0);

	while (running) {
		// keys (edge triggered)
		const KEY_Code_t key = KEYBOARD_Poll();
		if (key != lastKey) {
			if (key != KEY_INVALID && key != KEY_PTT)
				OnKey(key);
			lastKey = key;
		}

		RxPoll();

#ifdef ENABLE_VOICE
		if (voiceBusy) {
			if (gFlagPlayQueuedVoice) {
				gFlagPlayQueuedVoice = false;
				AUDIO_PlayQueuedVoice();
			}
			if (gVoiceReadIndex == 0 && gVoiceWriteIndex == 0) {
				voiceBusy = false;
				AudioRoute();                // the speaker back to MONITOR's say
			}
		}
#endif

		// every 10 ms: RSSI and floor; every 100 ms: the status line
		if (gNextTimeslice) {
			gNextTimeslice = false;
			tick++;
			RssiTick();
			if ((tick % 10) == 0)
				DrawStatus();
		}

		// a full redraw is ~20 ms of SPI: never while a burst is being sampled
		if (redraw && !sampling && !draining)
			Draw();

		SYSTEM_DelayMs(1);
	}

	// leave: ADC off, PA8 back to the UART, radio back to normal
	AdcDeinit();
	BK4819_WriteRegister(BK4819_REG_3F, 0);
	BK4819_WriteRegister(BK4819_REG_02, 0);
	ALERT_StoreConfig();
	SETTINGS_SaveSettings();
	RADIO_SetupRegisters(true);
	gRequestDisplayScreen = DISPLAY_MAIN;
	gUpdateStatus  = true;
	gUpdateDisplay = true;
}

#endif // ENABLE_ALERT
