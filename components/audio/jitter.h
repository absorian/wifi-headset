/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JITTER_MAX_NACK 32

typedef struct jitter_buffer jitter_buffer_t;

typedef enum {
	JITTER_FRAME_OK,
	JITTER_FRAME_CONCEAL,
	JITTER_FRAME_PREBUFFER,
} jitter_frame_t;

typedef enum {
	JITTER_CONCEAL_PCM,
	JITTER_CONCEAL_NONE,
} jitter_conceal_t;

jitter_buffer_t *jitter_create(uint32_t slot_size, uint8_t num_chan,
			       uint16_t slot_cap, uint16_t target_depth,
			       jitter_conceal_t conceal);
void jitter_destroy(jitter_buffer_t *jb);

void jitter_put(jitter_buffer_t *jb, uint16_t seqnum, const void *data,
		uint32_t len, uint16_t *nacks, uint32_t *n_nacks);

jitter_frame_t jitter_get(jitter_buffer_t *jb, void *out);
