/* ALERT telemetry receiver app for the Quansheng UV-K5 (DP32G030 + BK4819).
 *
 * Receives legacy ALERT (ERTS) flood-warning telemetry - 300 baud AFSK on
 * 1300 / 2100 Hz (V.23 mode 2 tones, measured off air), four 10-bit async
 * words carrying a 13-bit station address and an 11-bit value - decodes it,
 * looks the address up in the MegaNet-derived station table, shows it on the
 * LCD and optionally reads it out with the radio's voice prompts.
 *
 * The receiver's demodulated audio is sampled by the MCU's SAR ADC at 9600 Hz
 * while the squelch is open and demodulated in software (app/alert_demod.c),
 * then framed and checked (app/alert_decode.c). Both are the code that decodes
 * on air in quansheng_alert_v3, where the radio routes that audio to an ADC
 * pin by itself. This radio does not, so it needs a one-wire hardware mod:
 * BK4819 pin 8 (EARO) -> 100 nF -> DP32G030 pin 9 (PA8 / ADC CH3). See
 * README.md.
 *
 * There used to be a second input, the BK4819's own FSK engine used as a bit
 * slicer. The V3 work showed that engine never syncs on an ALERT burst - its
 * modes are fixed tone pairs and none is 1300/2100 at 300 baud - so it is gone.
 */
#ifndef APP_ALERT_H
#define APP_ALERT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ENABLE_ALERT

// Modal app: takes over the radio and the screen until EXIT is pressed.
void APP_RunAlert(void);

// Load / store the app's configuration bytes (gEeprom.ALERT_CFG).
void ALERT_LoadConfig(void);
void ALERT_StoreConfig(void);

// Called from the SysTick handler at 9600 Hz while gAlertAdcRun is set, which
// is only while a burst is being sampled.
extern volatile bool gAlertAdcRun;
void ALERT_AdcTick(void);

#endif

#endif
