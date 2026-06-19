/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_BUF_SIZE 512
#define NACK_MAX_SEQS 32

#define NACK_MAGIC_MASK 0xFF00
#define AUDIO_MAGIC_MASK 0x00FF
#define NACK_MAGIC 0x4900
#define AUDIO_MAGIC 0x00A3

typedef struct {
	uint16_t count;
	uint16_t seqs[NACK_MAX_SEQS];
} nack_pkt_t;

typedef struct {
	uint16_t seqnum;
	uint16_t size;
	uint8_t data[AUDIO_BUF_SIZE];
} audio_pkt_t;

#define PAYLOAD_MAX (sizeof(nack_pkt_t) + sizeof(audio_pkt_t))
typedef struct {
	uint16_t magic;
	uint8_t payload[PAYLOAD_MAX];
} pkt_t;

void pkt_init(pkt_t *pkt);

bool pkt_add_nack(pkt_t *pkt, const uint16_t *seqs, uint16_t count);

// Append audio (<= AUDIO_BUF_SIZE)
bool pkt_add_audio(pkt_t *pkt, uint16_t seqnum, const void *data,
		   uint16_t size);

uint32_t pkt_get_size(const pkt_t *pkt);

const nack_pkt_t *pkt_get_nacks(const pkt_t *pkt);
const audio_pkt_t *pkt_get_audio(const pkt_t *pkt);
