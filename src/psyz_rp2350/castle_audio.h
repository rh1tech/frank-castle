/*
 * frank-castle — the sound output (audio.c), for the firmware's own code.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef CASTLE_AUDIO_H
#define CASTLE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_OUT_HDMI 0 /* audio packets in the HDMI signal, 44.1 kHz */
#define AUDIO_OUT_I2S 1  /* the I2S DAC (GPIO 9-11) */

void Audio_Init(void);
void Audio_SetGain(uint32_t gain); /* 0..256 */
void Audio_Pause(bool on);
/* AUDIO_OUT_*: takes effect at once; the other output plays silence */
void Audio_SetOutput(int out);
int Audio_GetOutput(void);

extern volatile uint32_t audio_underruns, audio_hdmi_underruns;

#endif
