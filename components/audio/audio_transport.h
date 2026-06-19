/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include "jitter.h"

#include <stdint.h>

typedef struct {
	uint16_t local_port; // bind port, 0 = ephemeral
	const char *remote_ip;
	uint16_t remote_port; // 0 = symmetric (connect to the bound local port)

	uint32_t slot_size; // audio bytes per packet
	uint8_t num_chan;
	uint16_t jitter_cap; // jitter buffer slot count
	uint16_t jitter_target; // packets to prebuffer before playback
	uint16_t history_size; // retransmit history depth, 0 = no retransmit
} audio_transport_cfg_t;

int audio_transport_open(const audio_transport_cfg_t *cfg,
			 uint16_t *out_local_port);
void audio_transport_close(void);

void audio_transport_send(const void *audio, uint32_t size);

jitter_frame_t audio_transport_recv(void *out);
