/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"
#include "audio_transport.h"
#include "audio_config.h"
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

#define HISTORY_SIZE 64
#define JITTER_CAPACITY 64

static ma_context s_ma_ctx;
static ma_device s_spk_src_dev;

#if AUDIO_LC3_SPK_ENABLE
static lc3_encoder_t s_spk_enc[AUDIO_NUM_CHAN];
static void *s_spk_enc_mem[AUDIO_NUM_CHAN];
static int s_spk_frame_samples;
static int16_t *s_spk_acc; // interleaved accumulator
static int s_spk_acc_fill; // frames buffered
#endif

#if AUDIO_MIC_ENABLE
static ma_device s_mic_dest_dev;
static int16_t *s_mic_pcm; // decoded/raw mono frame
static int s_mic_samples; // samples per frame
static int s_mic_avail;
static int s_mic_pos;
static uint8_t s_mic_buf[AUDIO_BUF_SIZE];
#if AUDIO_LC3_MIC_ENABLE
static lc3_decoder_t s_mic_dec;
static void *s_mic_dec_mem;
#endif
#endif

static void audio_tx_thread(ma_device *pDevice, void *pOutput,
			    const void *pInput, ma_uint32 frameCount)
{
	(void)pDevice;
	(void)pOutput;

#if AUDIO_LC3_SPK_ENABLE
	const int16_t *pcm = pInput;
	ma_uint32 i = 0;

	while (i < frameCount) {
		int n = frameCount - i;
		if (n > s_spk_frame_samples - s_spk_acc_fill)
			n = s_spk_frame_samples - s_spk_acc_fill;

		memcpy(s_spk_acc + s_spk_acc_fill * AUDIO_NUM_CHAN,
		       pcm + i * AUDIO_NUM_CHAN, n * AUDIO_FRAME_SIZE);
		s_spk_acc_fill += n;
		i += n;

		if (s_spk_acc_fill < s_spk_frame_samples)
			break;

		uint8_t enc[AUDIO_NUM_CHAN * AUDIO_SPK_NBYTE];
		for (int ch = 0; ch < AUDIO_NUM_CHAN; ch++)
			lc3_encode(s_spk_enc[ch], LC3_PCM_FORMAT_S16,
				   s_spk_acc + ch, AUDIO_NUM_CHAN,
				   AUDIO_SPK_NBYTE, enc + ch * AUDIO_SPK_NBYTE);
		audio_transport_send(enc, sizeof(enc));
		s_spk_acc_fill = 0;
	}
#else
	audio_transport_send(pInput, frameCount * AUDIO_FRAME_SIZE);
#endif
}

#if AUDIO_MIC_ENABLE
static void audio_rx_thread(ma_device *pDevice, void *pOutput,
			    const void *pInput, ma_uint32 frameCount)
{
	int16_t *out = pOutput;
	ma_uint32 produced = 0;

	(void)pDevice;
	(void)pInput;

	while (produced < frameCount) {
		if (s_mic_pos >= s_mic_avail) {
			jitter_frame_t kind = audio_transport_recv(s_mic_buf);
#if AUDIO_LC3_MIC_ENABLE
			if (kind == JITTER_FRAME_PREBUFFER)
				memset(s_mic_pcm, 0,
				       s_mic_samples * sizeof(int16_t));
			else if (kind == JITTER_FRAME_OK)
				lc3_decode(s_mic_dec, s_mic_buf,
					   AUDIO_MIC_NBYTE, LC3_PCM_FORMAT_S16,
					   s_mic_pcm, 1);
			else
				lc3_decode(s_mic_dec, NULL, 0,
					   LC3_PCM_FORMAT_S16, s_mic_pcm, 1);
#else
			(void)kind;
			memcpy(s_mic_pcm, s_mic_buf,
			       s_mic_samples * sizeof(int16_t));
#endif
			s_mic_avail = s_mic_samples;
			s_mic_pos = 0;
		}

		ma_uint32 n = frameCount - produced;
		if (n > (ma_uint32)(s_mic_avail - s_mic_pos))
			n = s_mic_avail - s_mic_pos;

		memcpy(out + produced, s_mic_pcm + s_mic_pos,
		       n * sizeof(int16_t));
		produced += n;
		s_mic_pos += n;
	}
}
#endif

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

	if (playback == NULL)
		return;

	for (ma_uint32 i = 0; i < playbackCount; i += 1) {
		printf("%d - %s\n", i + 1, pPlaybackInfos[i].name);
	}
	choice = -1;
	while (choice <= 0 || choice > playbackCount) {
		printf("Select mic output: ");
		scanf("%d", &choice);
	}
	*playback = &pPlaybackInfos[choice - 1].id;
}

void audio_transport_start()
{
#if AUDIO_LC3_SPK_ENABLE
	s_spk_acc_fill = 0;
#endif
	ma_device_start(&s_spk_src_dev);
#if AUDIO_MIC_ENABLE
	s_mic_avail = 0;
	s_mic_pos = 0;
	ma_device_start(&s_mic_dest_dev);
#endif
}

void audio_transport_stop()
{
	ma_device_stop(&s_spk_src_dev);
#if AUDIO_MIC_ENABLE
	ma_device_stop(&s_mic_dest_dev);
#endif
}

int audio_transport_open_conn(cl_conn_info_t *conn_info)
{
	audio_transport_cfg_t cfg = {
		.local_port = 0,
		.remote_ip = inet_ntoa(conn_info->addr.sin_addr),
		.remote_port = 0,
		.num_chan = 1,
		.jitter_enable = AUDIO_JITTER_ENABLE,
		.jitter_cap = JITTER_CAPACITY,
		.jitter_target = AUDIO_JITTER_DEPTH,
#if AUDIO_LC3_MIC_ENABLE
		.slot_size = AUDIO_MIC_NBYTE,
		.conceal = JITTER_CONCEAL_NONE,
#else
		.slot_size = AUDIO_BUF_SIZE,
		.conceal = JITTER_CONCEAL_PCM,
#endif
		.history_size = AUDIO_JITTER_ENABLE ? HISTORY_SIZE : 0,
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

#if AUDIO_LC3_SPK_ENABLE
static int spk_enc_init(void)
{
	s_spk_frame_samples =
		lc3_frame_samples(AUDIO_LC3_FRAME_US, AUDIO_SAMPLE_RATE);
	if (s_spk_frame_samples < 0)
		return -1;

	s_spk_acc = malloc(s_spk_frame_samples * AUDIO_FRAME_SIZE);
	if (s_spk_acc == NULL)
		return -1;

	for (int ch = 0; ch < AUDIO_NUM_CHAN; ch++) {
		s_spk_enc_mem[ch] = malloc(lc3_encoder_size(AUDIO_LC3_FRAME_US,
							    AUDIO_SAMPLE_RATE));
		if (s_spk_enc_mem[ch] == NULL)
			return -1;
		s_spk_enc[ch] =
			lc3_setup_encoder(AUDIO_LC3_FRAME_US, AUDIO_SAMPLE_RATE,
					  AUDIO_SAMPLE_RATE, s_spk_enc_mem[ch]);
		if (s_spk_enc[ch] == NULL)
			return -1;
	}
	return 0;
}
#endif

#if AUDIO_MIC_ENABLE
static int mic_dec_init(void)
{
#if AUDIO_LC3_MIC_ENABLE
	s_mic_samples =
		lc3_frame_samples(AUDIO_LC3_FRAME_US, AUDIO_SAMPLE_RATE);
	if (s_mic_samples < 0)
		return -1;
	s_mic_dec_mem =
		malloc(lc3_decoder_size(AUDIO_LC3_FRAME_US, AUDIO_SAMPLE_RATE));
	if (s_mic_dec_mem == NULL)
		return -1;
	s_mic_dec = lc3_setup_decoder(AUDIO_LC3_FRAME_US, AUDIO_SAMPLE_RATE,
				      AUDIO_SAMPLE_RATE, s_mic_dec_mem);
	if (s_mic_dec == NULL)
		return -1;
#else
	s_mic_samples = AUDIO_BUF_SIZE / sizeof(int16_t);
#endif
	s_mic_pcm = malloc(s_mic_samples * sizeof(int16_t));
	return s_mic_pcm != NULL ? 0 : -1;
}
#endif

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

#if AUDIO_LC3_SPK_ENABLE
	if (spk_enc_init() != 0) {
		fprintf(stderr, "Failed to init LC3 encoder\n");
		return -1;
	}
#endif
#if AUDIO_MIC_ENABLE
	if (mic_dec_init() != 0) {
		fprintf(stderr, "Failed to init mic decoder\n");
		return -1;
	}
	audio_transport_select_dev(&capture_id, &playback_id);
#else
	audio_transport_select_dev(&capture_id, NULL);
#endif

	config = ma_device_config_init(ma_device_type_capture);
#if AUDIO_LC3_SPK_ENABLE
	config.periodSizeInFrames = s_spk_frame_samples;
#else
	config.periodSizeInFrames = AUDIO_BUF_SIZE / AUDIO_FRAME_SIZE;
#endif
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

#if AUDIO_MIC_ENABLE
	config = ma_device_config_init(ma_device_type_playback);
	config.periodSizeInFrames = s_mic_samples;
	config.playback.format = ma_format_s16;
	config.playback.channels = 1;
	config.sampleRate = AUDIO_SAMPLE_RATE;
	config.dataCallback = audio_rx_thread;
	config.playback.pDeviceID = playback_id;
	ret = ma_device_init(&s_ma_ctx, &config, &s_mic_dest_dev);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init mic_dest ma_device: %d\n", ret);
		return -1;
	}
#endif

	return 0;
}

void audio_transport_deinit()
{
	ma_device_uninit(&s_spk_src_dev);
#if AUDIO_MIC_ENABLE
	ma_device_uninit(&s_mic_dest_dev);
#endif
	ma_context_uninit(&s_ma_ctx);

#if AUDIO_LC3_SPK_ENABLE
	free(s_spk_acc);
	for (int ch = 0; ch < AUDIO_NUM_CHAN; ch++)
		free(s_spk_enc_mem[ch]);
#endif
#if AUDIO_MIC_ENABLE
	free(s_mic_pcm);
#if AUDIO_LC3_MIC_ENABLE
	free(s_mic_dec_mem);
#endif
#endif
}
