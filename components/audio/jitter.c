/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "jitter.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JITTER_CONCEAL_FADE_STEP 0.125f

struct slot {
	bool valid;
	uint16_t seq;
	uint32_t len;
	uint8_t *data;
};

struct jitter_buffer {
	uint32_t slot_size;
	uint8_t num_chan;
	uint16_t slot_cap;
	uint16_t target_diff;

	bool started; // prebuffer satisfied, playback running
	bool anchored; // read_seq/write_seq are valid
	uint16_t read_seq; // next sequence number to play
	uint16_t write_seq; // highest sequence number inserted
	uint16_t slots_count; // number of valid slots

	uint8_t *last_slot; // most recent good frame, for concealment
	bool have_last; // last_frame holds real audio
	bool was_concealed; // previous output was concealed/silence
	float conceal_gain; // current fade-out envelope level (1.0 = full)

	pthread_mutex_t mtx;
	struct slot *slots;
	uint8_t *storage;
};

static inline int seq_diff(uint16_t a, uint16_t b)
{
	return (int16_t)(a - b);
}

static void apply_gain_ramp(void *data, uint32_t size, uint8_t num_chan,
			    float g0, float g1)
{
	int16_t *s = data;
	uint32_t num_frames = size / (sizeof(int16_t) * num_chan);

	if (num_frames == 0)
		return;

	for (int fr = 0; fr < num_frames; fr++) {
		float g = g0 + (g1 - g0) * ((float)fr / (float)num_frames);
		int i = fr * num_chan;

		for (int ch = 0; ch < num_chan; ch++) {
			s[i + ch] = (int16_t)(s[i + ch] * g);
		}
	}
}

static void conceal_into(jitter_buffer_t *jb, void *out)
{
	float g0, g1;

	jb->was_concealed = true;

	if (!jb->have_last || jb->conceal_gain <= 0.0f) {
		memset(out, 0, jb->slot_size);
		jb->conceal_gain = 0.0f;
		return;
	}

	g0 = jb->conceal_gain;
	g1 = g0 - JITTER_CONCEAL_FADE_STEP;
	if (g1 < 0.0f)
		g1 = 0.0f;

	memcpy(out, jb->last_slot, jb->slot_size);
	apply_gain_ramp(out, jb->slot_size, jb->num_chan, g0, g1);
	jb->conceal_gain = g1;
}

jitter_buffer_t *jitter_create(uint32_t slot_size, uint8_t num_chan,
			       uint16_t slot_cap, uint16_t target_depth)
{
	jitter_buffer_t *jb;

	if (slot_size == 0 || slot_cap == 0 || target_depth >= slot_cap)
		return NULL;

	jb = calloc(1, sizeof(*jb));
	if (jb == NULL)
		return NULL;

	jb->slot_size = slot_size;
	jb->num_chan = num_chan;
	jb->slot_cap = slot_cap;
	jb->target_diff = target_depth;
	jb->conceal_gain = 1.0f;

	if (pthread_mutex_init(&jb->mtx, NULL) != 0) {
		jitter_destroy(jb);
		return NULL;
	}

	jb->slots = calloc(slot_cap, sizeof(*jb->slots));
	jb->storage = calloc(slot_cap, slot_size);
	jb->last_slot = calloc(1, slot_size);
	if (jb->slots == NULL || jb->storage == NULL || jb->last_slot == NULL) {
		jitter_destroy(jb);
		return NULL;
	}

	for (uint16_t i = 0; i < slot_cap; i++)
		jb->slots[i].data = jb->storage + (uint32_t)i * slot_size;

	return jb;
}

void jitter_destroy(jitter_buffer_t *jb)
{
	if (jb == NULL)
		return;

	pthread_mutex_destroy(&jb->mtx);
	free(jb->slots);
	free(jb->storage);
	free(jb->last_slot);
	free(jb);
}

void jitter_put(jitter_buffer_t *jb, uint16_t seqnum, const void *data,
		uint32_t len, uint16_t *nacks, uint32_t *n_nacks)
{
	struct slot *s;

	*n_nacks = 0;

	if (len > jb->slot_size)
		len = jb->slot_size;

	pthread_mutex_lock(&jb->mtx);

	if (!jb->anchored) {
		jb->anchored = true;
		jb->started = false;
		jb->read_seq = seqnum;
		jb->write_seq = seqnum;
	}

	if (jb->started && seq_diff(seqnum, jb->read_seq) < 0)
		goto out;

	if (seq_diff(seqnum, jb->read_seq) >= jb->slot_cap)
		goto out;

	if (seq_diff(seqnum, jb->write_seq) > 1) {
		for (uint16_t miss = jb->write_seq + 1;
		     seq_diff(seqnum, miss) > 0; miss++) {
			if (jb->started && seq_diff(miss, jb->read_seq) < 0)
				continue;
			if (*n_nacks >= JITTER_MAX_NACK)
				break;
			nacks[(*n_nacks)++] = miss;
		}
	}

	s = &jb->slots[seqnum % jb->slot_cap];
	if (!s->valid || s->seq != seqnum) {
		if (!s->valid)
			jb->slots_count++;
		s->valid = true;
		s->seq = seqnum;
	}
	s->len = len;
	memcpy(s->data, data, len);
	if (len < jb->slot_size)
		memset(s->data + len, 0, jb->slot_size - len);

	if (seq_diff(seqnum, jb->write_seq) > 0)
		jb->write_seq = seqnum;

out:
	pthread_mutex_unlock(&jb->mtx);
}

jitter_frame_t jitter_get(jitter_buffer_t *jb, void *out)
{
	struct slot *s;

	pthread_mutex_lock(&jb->mtx);

	if (!jb->started) {
		if (!jb->anchored || jb->slots_count < jb->target_diff) {
			conceal_into(jb, out);
			pthread_mutex_unlock(&jb->mtx);
			return JITTER_FRAME_PREBUFFER;
		}
		jb->started = true;
	}

	s = &jb->slots[jb->read_seq % jb->slot_cap];
	if (s->valid && s->seq == jb->read_seq) {
		memcpy(out, s->data, jb->slot_size);
		s->valid = false;
		jb->slots_count--;
		jb->read_seq++;

		memcpy(jb->last_slot, out, jb->slot_size);
		jb->have_last = true;
		if (jb->was_concealed && jb->conceal_gain < 1.0f)
			apply_gain_ramp(out, jb->slot_size, jb->num_chan,
					jb->conceal_gain, 1.0f);
		jb->conceal_gain = 1.0f;
		jb->was_concealed = false;

		pthread_mutex_unlock(&jb->mtx);
		return JITTER_FRAME_OK;
	}

	if (jb->slots_count == 0) {
		jb->started = false;
		jb->anchored = false;
		conceal_into(jb, out);
		pthread_mutex_unlock(&jb->mtx);
		return JITTER_FRAME_PREBUFFER;
	}

	jb->read_seq++;
	conceal_into(jb, out);
	pthread_mutex_unlock(&jb->mtx);
	return JITTER_FRAME_CONCEAL;
}
