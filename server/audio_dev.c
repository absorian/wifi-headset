/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"
#include "audio_transport.h"
#include "pkt.h"
#include <miniaudio.h>
#include <lc3.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_NUM_CHAN 2
#define AUDIO_FRAME_SIZE (sizeof(int16_t) * AUDIO_NUM_CHAN)
#define AUDIO_SAMPLE_RATE 48000

#define HISTORY_SIZE 64
#define JITTER_CAPACITY 64
#define JITTER_TARGET_DEPTH 3

#define MIC_SAMPLE_RATE 48000
#define MIC_DT_US 7500
#define MIC_NBYTE 90

static ma_context s_ma_ctx;
static ma_device s_spk_src_dev;
static ma_device s_mic_dest_dev;

static lc3_decoder_t s_dec;
static void *s_dec_mem;
static int s_frame_samples; // PCM samples produced per LC3 frame
static int16_t *s_dec_pcm; // one decoded frame
static int s_dec_avail; // samples in s_dec_pcm
static int s_dec_pos; // samples already consumed
static uint8_t s_lc3_buf[AUDIO_BUF_SIZE];

static void audio_tx_thread(ma_device *pDevice, void *pOutput,
			    const void *pInput, ma_uint32 frameCount)
{
	(void)pDevice;
	(void)pOutput;

	audio_transport_send(pInput, frameCount * AUDIO_FRAME_SIZE);
}

static void audio_rx_thread(ma_device *pDevice, void *pOutput,
			    const void *pInput, ma_uint32 frameCount)
{
	int16_t *out = pOutput;
	ma_uint32 produced = 0;

	(void)pDevice;
	(void)pInput;

	while (produced < frameCount) {
		if (s_dec_pos >= s_dec_avail) {
			jitter_frame_t kind = audio_transport_recv(s_lc3_buf);

			if (kind == JITTER_FRAME_OK)
				lc3_decode(s_dec, s_lc3_buf, MIC_NBYTE,
					   LC3_PCM_FORMAT_S16, s_dec_pcm, 1);
			else
				lc3_decode(s_dec, NULL, 0, LC3_PCM_FORMAT_S16,
					   s_dec_pcm, 1); // PLC
			s_dec_avail = s_frame_samples;
			s_dec_pos = 0;
		}

		ma_uint32 n = frameCount - produced;
		if (n > (ma_uint32)(s_dec_avail - s_dec_pos))
			n = s_dec_avail - s_dec_pos;

		memcpy(out + produced, s_dec_pcm + s_dec_pos,
		       n * sizeof(int16_t));
		produced += n;
		s_dec_pos += n;
	}
}

static void audio_transport_select_dev(const ma_device_id **capture,
				       const ma_device_id **playback)
{
	ma_result ret;
	ma_device_info *pPlaybackInfos;
	ma_uint32 playbackCount;
	ma_device_info *pCaptureInfos;
	ma_uint32 captureCount;
	int choice;

	ret = ma_context_get_devices(&s_ma_ctx, &pPlaybackInfos, &playbackCount,
				     &pCaptureInfos, &captureCount);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to get audio devices: %d\n", ret);
		return;
	}

	for (ma_uint32 i = 0; i < captureCount; i += 1) {
		printf("%d - %s\n", i + 1, pCaptureInfos[i].name);
	}
	choice = -1;
	while (choice <= 0 || choice > captureCount) {
		printf("Select speaker source: ");
		scanf("%d", &choice);
	}
	if (capture != NULL)
		*capture = &pCaptureInfos[choice - 1].id;

	for (ma_uint32 i = 0; i < playbackCount; i += 1) {
		printf("%d - %s\n", i + 1, pPlaybackInfos[i].name);
	}
	choice = -1;
	while (choice <= 0 || choice > playbackCount) {
		printf("Select mic output: ");
		scanf("%d", &choice);
	}
	if (playback != NULL)
		*playback = &pPlaybackInfos[choice - 1].id;
}

void audio_transport_start()
{
	s_dec_avail = 0;
	s_dec_pos = 0;

	ma_device_start(&s_spk_src_dev);
	ma_device_start(&s_mic_dest_dev);
}

void audio_transport_stop()
{
	ma_device_stop(&s_spk_src_dev);
	ma_device_stop(&s_mic_dest_dev);
}

int audio_transport_open_conn(cl_conn_info_t *conn_info)
{
	audio_transport_cfg_t cfg = {
		.local_port = 0, // ephemeral
		.remote_ip = inet_ntoa(conn_info->addr.sin_addr),
		.remote_port = 0, // symmetric: send to our bound port
		.slot_size = MIC_NBYTE, // inbound mic, LC3 encoded
		.num_chan = 1,
		.jitter_cap = JITTER_CAPACITY,
		.jitter_target = JITTER_TARGET_DEPTH,
		.history_size = HISTORY_SIZE, // retransmit speaker on NACK
		.conceal = JITTER_CONCEAL_NONE, // LC3 does its own PLC
	};

	if (audio_transport_open(&cfg, &conn_info->audio_port) != 0) {
		fprintf(stderr, "Failed to open audio transport\n");
		return -1;
	}

	return 0;
}

void audio_transport_close_conn()
{
	audio_transport_close();
}

static int mic_dec_init(void)
{
	s_frame_samples = lc3_frame_samples(MIC_DT_US, MIC_SAMPLE_RATE);
	if (s_frame_samples < 0)
		return -1;

	s_dec_mem = malloc(lc3_decoder_size(MIC_DT_US, MIC_SAMPLE_RATE));
	s_dec_pcm = malloc(s_frame_samples * sizeof(int16_t));
	if (s_dec_mem == NULL || s_dec_pcm == NULL)
		return -1;

	s_dec = lc3_setup_decoder(MIC_DT_US, MIC_SAMPLE_RATE, MIC_SAMPLE_RATE,
				  s_dec_mem);
	return s_dec != NULL ? 0 : -1;
}

int audio_transport_init()
{
	const ma_device_id *capture_id = NULL;
	const ma_device_id *playback_id = NULL;
	ma_device_config config;
	ma_result ret;

	ret = ma_context_init(NULL, 0, NULL, &s_ma_ctx);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init ma_context: %d\n", ret);
		return -1;
	}

	if (mic_dec_init() != 0) {
		fprintf(stderr, "Failed to init LC3 decoder\n");
		return -1;
	}

	audio_transport_select_dev(&capture_id, &playback_id);

	// Capture device: raw PCM downlink to the headset.
	config = ma_device_config_init(ma_device_type_capture);
	config.periodSizeInFrames = AUDIO_BUF_SIZE / AUDIO_FRAME_SIZE;
	config.capture.format = ma_format_s16;
	config.capture.channels = AUDIO_NUM_CHAN;
	config.sampleRate = AUDIO_SAMPLE_RATE;
	config.dataCallback = audio_tx_thread;
	config.capture.pDeviceID = capture_id;
	ret = ma_device_init(&s_ma_ctx, &config, &s_spk_src_dev);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init spk_src ma_device: %d\n", ret);
		return -1;
	}

	// Playback device: decoded mic uplink.
	config = ma_device_config_init(ma_device_type_playback);
	config.periodSizeInFrames = s_frame_samples;
	config.playback.format = ma_format_s16;
	config.playback.channels = 1;
	config.sampleRate = MIC_SAMPLE_RATE;
	config.dataCallback = audio_rx_thread;
	config.playback.pDeviceID = playback_id;
	ret = ma_device_init(&s_ma_ctx, &config, &s_mic_dest_dev);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init mic_dest ma_device: %d\n", ret);
		return -1;
	}

	return 0;
}

void audio_transport_deinit()
{
	ma_device_uninit(&s_spk_src_dev);
	ma_device_uninit(&s_mic_dest_dev);
	ma_context_uninit(&s_ma_ctx);

	free(s_dec_mem);
	free(s_dec_pcm);
	s_dec_mem = NULL;
	s_dec_pcm = NULL;
}
