/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "pkt.h"

#include <stddef.h>
#include <string.h>

static inline bool pkt_has_nack(const pkt_t *pkt)
{
	return (pkt->magic & NACK_MAGIC_MASK) == NACK_MAGIC;
}

static inline bool pkt_has_audio(const pkt_t *pkt)
{
	return (pkt->magic & AUDIO_MAGIC_MASK) == AUDIO_MAGIC;
}

static uint32_t nack_block_size(const pkt_t *pkt)
{
	const nack_pkt_t *nack = (const nack_pkt_t *)pkt->payload;

	if (!pkt_has_nack(pkt))
		return 0;
	return offsetof(nack_pkt_t, seqs) + nack->count * sizeof(uint16_t);
}

void pkt_init(pkt_t *pkt)
{
	pkt->magic = 0;
}

bool pkt_add_nack(pkt_t *pkt, const uint16_t *seqs, uint16_t count)
{
	nack_pkt_t *nack = (nack_pkt_t *)pkt->payload;

	// nack must be first
	if (pkt_has_audio(pkt))
		return false;

	if (count > NACK_MAX_SEQS)
		count = NACK_MAX_SEQS;

	nack->count = count;
	memcpy(nack->seqs, seqs, count * sizeof(uint16_t));

	pkt->magic = (pkt->magic & ~NACK_MAGIC_MASK) | NACK_MAGIC;
	return true;
}

bool pkt_add_audio(pkt_t *pkt, uint16_t seqnum, const void *data, uint16_t size)
{
	audio_pkt_t *audio;

	if (size > AUDIO_BUF_SIZE)
		return false;

	audio = (audio_pkt_t *)(pkt->payload + nack_block_size(pkt));
	audio->seqnum = seqnum;
	audio->size = size;
	memcpy(audio->data, data, size);

	pkt->magic = (pkt->magic & ~AUDIO_MAGIC_MASK) | AUDIO_MAGIC;
	return true;
}

uint32_t pkt_get_size(const pkt_t *pkt)
{
	uint32_t size = offsetof(pkt_t, payload);
	const audio_pkt_t *audio;

	size += nack_block_size(pkt);
	if (pkt_has_audio(pkt)) {
		audio = (const audio_pkt_t *)(pkt->payload +
					      nack_block_size(pkt));
		size += offsetof(audio_pkt_t, data) + audio->size;
	}
	return size;
}

const nack_pkt_t *pkt_get_nacks(const pkt_t *pkt)
{
	if (!pkt_has_nack(pkt))
		return NULL;
	return (const nack_pkt_t *)pkt->payload;
}

const audio_pkt_t *pkt_get_audio(const pkt_t *pkt)
{
	if (!pkt_has_audio(pkt))
		return NULL;
	return (const audio_pkt_t *)(pkt->payload + nack_block_size(pkt));
}
