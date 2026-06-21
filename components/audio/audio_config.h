/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

// Feature toggles. The mic and the two LC3 flags define the wire format and
// must match on both sides; the jitter flag/depth are per-receiver.
#define AUDIO_MIC_ENABLE 1
#define AUDIO_LC3_SPK_ENABLE 1 // server -> device speaker
#define AUDIO_LC3_MIC_ENABLE 1 // device -> server mic
#define AUDIO_JITTER_ENABLE 1 // both directions
#define AUDIO_JITTER_DEPTH 3

// Shared audio/LC3 wire parameters.
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_LC3_FRAME_US 10000 // 10 ms
#define AUDIO_SPK_NBYTE 120 // LC3 bytes per channel, speaker
#define AUDIO_MIC_NBYTE 120 // LC3 bytes, mic (mono)
