/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "audio_transport.h"
#include "pkt.h"

#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include "lwip/inet.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

typedef struct {
	bool valid;
	audio_pkt_t pkt;
} hist_entry_t;

static int s_sock = -1;

static jitter_buffer_t *s_jb;

static hist_entry_t *s_history;
static uint16_t s_history_size;
static uint16_t s_tx_seq; // next outbound audio seqnum

static pthread_t s_rx_thread;
static atomic_int s_rx_run;

// FIFO ring used when the jitter buffer is disabled: plays frames in arrival
// order, absorbing UDP bursts instead of dropping them. No prebuffer, reorder
// or PLC, so it underruns to silence on late packets.
#define PASS_RING_DEPTH 16
static bool s_jitter_enable;
static uint8_t *s_pass_ring;
static uint32_t s_pass_slot;
static int s_pass_head;
static int s_pass_tail;
static int s_pass_count;
static pthread_mutex_t s_pass_mtx = PTHREAD_MUTEX_INITIALIZER;

static void passthrough_put(const void *data, uint32_t len)
{
	uint8_t *slot;

	if (len > s_pass_slot)
		len = s_pass_slot;

	pthread_mutex_lock(&s_pass_mtx);
	if (s_pass_count == PASS_RING_DEPTH) {
		s_pass_head = (s_pass_head + 1) % PASS_RING_DEPTH;
		s_pass_count--;
	}
	slot = s_pass_ring + (size_t)s_pass_tail * s_pass_slot;
	memcpy(slot, data, len);
	if (len < s_pass_slot)
		memset(slot + len, 0, s_pass_slot - len);
	s_pass_tail = (s_pass_tail + 1) % PASS_RING_DEPTH;
	s_pass_count++;
	pthread_mutex_unlock(&s_pass_mtx);
}

static jitter_frame_t passthrough_get(void *out)
{
	jitter_frame_t r;

	pthread_mutex_lock(&s_pass_mtx);
	if (s_pass_count == 0) {
		memset(out, 0, s_pass_slot);
		r = JITTER_FRAME_PREBUFFER;
	} else {
		memcpy(out, s_pass_ring + (size_t)s_pass_head * s_pass_slot,
		       s_pass_slot);
		s_pass_head = (s_pass_head + 1) % PASS_RING_DEPTH;
		s_pass_count--;
		r = JITTER_FRAME_OK;
	}
	pthread_mutex_unlock(&s_pass_mtx);
	return r;
}

static void pkt_send(const pkt_t *pkt)
{
	send(s_sock, pkt, pkt_get_size(pkt), 0);
}

static void history_put(uint16_t seq, const void *audio, uint32_t size)
{
	hist_entry_t *e;

	if (s_history == NULL)
		return;

	e = &s_history[seq % s_history_size];
	e->valid = true;
	e->pkt.seqnum = seq;
	e->pkt.size = size;
	memcpy(e->pkt.data, audio, size);
}

static void history_resend_nacks(const nack_pkt_t *nack)
{
	hist_entry_t *e;
	pkt_t pkt;

	if (s_history == NULL)
		return;

	for (uint16_t i = 0; i < nack->count; i++) {
		e = &s_history[nack->seqs[i] % s_history_size];
		if (!e->valid || e->pkt.seqnum != nack->seqs[i])
			return; // too old to recover

		pkt_init(&pkt);
		pkt_add_audio(&pkt, e->pkt.seqnum, e->pkt.data, e->pkt.size);
		pkt_send(&pkt);
	}
}

static void nack_send(const uint16_t *seqs, uint32_t count)
{
	pkt_t pkt;

	pkt_init(&pkt);
	pkt_add_nack(&pkt, seqs, count);

	pkt_send(&pkt);
}

void audio_transport_send(const void *audio, uint32_t size)
{
	pkt_t pkt;

	pkt_init(&pkt);
	pkt_add_audio(&pkt, s_tx_seq, audio, size);
	pkt_send(&pkt);
	history_put(s_tx_seq, audio, size);
	s_tx_seq++;
}

jitter_frame_t audio_transport_recv(void *out)
{
	if (!s_jitter_enable)
		return passthrough_get(out);
	return jitter_get(s_jb, out);
}

static void *rx_thread(void *arg)
{
	(void)arg;
	pkt_t pkt;
	const nack_pkt_t *nack;
	const audio_pkt_t *audio;
	uint16_t nacks[JITTER_MAX_NACK];
	uint32_t n_nacks;
	int ret;

	while (s_rx_run) {
		ret = recv(s_sock, &pkt, sizeof(pkt), 0);
		if (ret <= 0) {
			if (ret < 0 && errno == EAGAIN)
				continue;
			break;
		}
		if (ret < (int)sizeof(pkt.magic))
			continue;

		nack = pkt_get_nacks(&pkt);
		if (nack != NULL)
			history_resend_nacks(nack);

		audio = pkt_get_audio(&pkt);
		if (audio != NULL) {
			if (!s_jitter_enable) {
				passthrough_put(audio->data, audio->size);
				continue;
			}
			jitter_put(s_jb, audio->seqnum, audio->data,
				   audio->size, nacks, &n_nacks);
			if (n_nacks > 0)
				nack_send(nacks, n_nacks);
		}
	}

	return NULL;
}

int audio_transport_open(const audio_transport_cfg_t *cfg,
			 uint16_t *out_local_port)
{
	struct sockaddr_in addr;
	struct timeval timeout;
	socklen_t socklen;
	uint16_t local_port;
	uint16_t remote_port;
	int ret = 0;

	if (s_sock >= 0)
		return -1;

	s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (s_sock < 0)
		return -1;

	timeout.tv_sec = 0;
	timeout.tv_usec = 100 * 1000;
	ret = setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			 sizeof timeout);
	if (ret < 0)
		goto err;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(cfg->local_port);
	ret = bind(s_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		goto err;

	socklen = sizeof(addr);
	ret = getsockname(s_sock, (struct sockaddr *)&addr, &socklen);
	if (ret < 0)
		goto err;
	local_port = ntohs(addr.sin_port);

	remote_port = cfg->remote_port ? cfg->remote_port : local_port;
	addr.sin_addr.s_addr = inet_addr(cfg->remote_ip);
	addr.sin_port = htons(remote_port);
	ret = connect(s_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		goto err;

	s_jitter_enable = cfg->jitter_enable;
	if (s_jitter_enable) {
		s_jb = jitter_create(cfg->slot_size, cfg->num_chan,
				     cfg->jitter_cap, cfg->jitter_target,
				     cfg->conceal);
		if (s_jb == NULL)
			goto err;
	} else {
		s_pass_slot = cfg->slot_size;
		s_pass_head = 0;
		s_pass_tail = 0;
		s_pass_count = 0;
		s_pass_ring = calloc(PASS_RING_DEPTH, s_pass_slot);
		if (s_pass_ring == NULL)
			goto err;
	}

	s_history_size = cfg->history_size;
	if (s_history_size > 0) {
		s_history = calloc(s_history_size, sizeof(*s_history));
		if (s_history == NULL)
			goto err;
	}
	s_tx_seq = rand();

	if (out_local_port != NULL)
		*out_local_port = local_port;

	s_rx_run = 1;
	if (pthread_create(&s_rx_thread, NULL, rx_thread, NULL) != 0) {
		s_rx_run = 0;
		goto err;
	}

	return 0;

err:
	jitter_destroy(s_jb);
	s_jb = NULL;
	free(s_history);
	s_history = NULL;
	free(s_pass_ring);
	s_pass_ring = NULL;
	close(s_sock);
	s_sock = -1;
	return -1;
}

void audio_transport_close(void)
{
	if (s_sock < 0)
		return;

	s_rx_run = 0;
	pthread_join(s_rx_thread, NULL);

	close(s_sock);
	s_sock = -1;

	jitter_destroy(s_jb);
	s_jb = NULL;
	free(s_history);
	s_history = NULL;
	free(s_pass_ring);
	s_pass_ring = NULL;
}
