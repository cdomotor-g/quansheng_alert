/* ALERT receiver: software AFSK demodulator - MCU ADC samples in, bits out.
 *
 * Copyright 2026 cdomotor-g. Apache-2.0, like the egzumer base it lives in.
 *
 * The same integer arithmetic as quansheng_alert_v3's DemodSample
 * (App/app/alert_adc.c there), which decoded 22 bursts out of 22 on air. That
 * one was itself ported from this repository's ALERT_AdcTick, then fixed:
 *
 *   - the tones are 1300 / 2100 Hz (V.23 mode 2, measured off air), not the
 *     Bell-202 1200 / 2200 Hz this code used to assume; 2100 Hz reads as 1;
 *   - each bit is taken half a bit after the transitions the clock locks to,
 *     not on them (61% of bursts to 100%);
 *   - the correlator input is clamped, so a DC step cannot wrap the int16
 *     products;
 *   - the DC tracker starts at each burst's first sample instead of at
 *     mid-rail, so a pin that sits far from mid-rail does not feed the
 *     correlators a step for the first ~100 ms.
 *
 * No hardware in here: alert.c feeds it one 12-bit sample per call at
 * ALERT_DEMOD_FS, and test/test_alert_demod.c feeds it synthesised bursts.
 */
#ifndef APP_ALERT_DEMOD_H
#define APP_ALERT_DEMOD_H

#include <stdint.h>

#define ALERT_DEMOD_FS    9600u            // sample rate, Hz
#define ALERT_DEMOD_BYTES 96u              // 768 bits = 2.56 s at 300 baud
#define ALERT_DEMOD_BITS  (ALERT_DEMOD_BYTES * 8u)

// Start of a burst: fresh correlators, bit clock and buffer; the DC tracker
// takes the next sample as its starting point.
void ALERT_DemodReset(void);

// One ADC sample (0..4095). Safe to call from an interrupt.
void ALERT_DemodSample(int32_t raw);

// The bits so far, MSB first in each byte (ALERT_GetBit order), one per ALERT
// bit. Read them only once the sampling has stopped.
uint16_t       ALERT_DemodBitCount(void);
const uint8_t *ALERT_DemodBits(void);

// The raw input level over the burst: lowest, highest and mean sample. A pin
// with no audio on it shows a few counts of spread; one pinned at 0 or 4095
// is clipping. 0 / 0 / 0 before the first sample.
void ALERT_DemodLevels(uint16_t *min, uint16_t *max, uint16_t *mean);

#endif
