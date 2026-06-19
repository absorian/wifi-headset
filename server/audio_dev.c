/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Server audio device glue: captures from a miniaudio input device and hands
 * frames to the shared audio transport component, which owns the socket, the
 * receive thread, retransmission and the jitter buffer.
 */

#include "common.h"
#include "audio_transport.h"
#include "pkt.h"
#include <miniaudio.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define AUDIO_NUM_CHAN 2
#define AUDIO_FRAME_SIZE (sizeof(int16_t) * AUDIO_NUM_CHAN)

/* Retransmit history depth and jitter buffer geometry. */
#define HISTORY_SIZE 256
#define JITTER_CAPACITY 64
#define JITTER_TARGET_DEPTH 3

static ma_context s_ma_ctx;
static ma_device s_spk_src_dev;
static ma_device s_mic_dest_dev;

static void audio_tx_thread(ma_device *pDevice, void *pOutput,
			    const void *pInput, ma_uint32 frameCount)
{
	(void)pDevice;
	(void)pOutput;

	audio_transport_send(pInput, frameCount * AUDIO_FRAME_SIZE);
}

static void audio_transport_select_dev(const ma_device_id **capture,
				       const ma_device_id **playback)
{
	ma_result ret;
	ma_device_info *pPlaybackInfos;
	ma_uint32 playbackCount;
	ma_device_info *pCaptureInfos;
	ma_uint32 captureCount;
	int choice = -1;

	ret = ma_context_get_devices(&s_ma_ctx, &pPlaybackInfos, &playbackCount,
				     &pCaptureInfos, &captureCount);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to get audio devices: %d\n", ret);
		return;
	}

	for (ma_uint32 i = 0; i < captureCount; i += 1) {
		printf("%d - %s\n", i + 1, pCaptureInfos[i].name);
	}
	while (choice <= 0 || choice > captureCount) {
		printf("Select speaker source: ");
		scanf("%d", &choice);
	}

	if (capture != NULL)
		*capture = &pCaptureInfos[choice - 1].id;
}

void audio_transport_start()
{
	ma_device_start(&s_spk_src_dev);
}

void audio_transport_stop()
{
	ma_device_stop(&s_spk_src_dev);
}

int audio_transport_open_conn(cl_conn_info_t *conn_info)
{
	audio_transport_cfg_t cfg = {
		.local_port = 0, // ephemeral
		.remote_ip = inet_ntoa(conn_info->addr.sin_addr),
		.remote_port = 0, // symmetric: send to our bound port
		.slot_size = AUDIO_BUF_SIZE,
		.num_chan = AUDIO_NUM_CHAN,
		.jitter_cap = JITTER_CAPACITY,
		.jitter_target = JITTER_TARGET_DEPTH,
		.history_size = HISTORY_SIZE,
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

int audio_transport_init()
{
	ma_result ret;
	ma_device_config config = ma_device_config_init(ma_device_type_capture);
	ret = ma_context_init(NULL, 0, NULL, &s_ma_ctx);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init ma_context: %d\n", ret);
		return -1;
	}

	config.periodSizeInFrames = AUDIO_BUF_SIZE / AUDIO_FRAME_SIZE;
	// Set to 0 to use the device's native channel count.
	config.capture.format = ma_format_s16;
	// Set to 0 to use the device's native channel count.
	config.capture.channels = AUDIO_NUM_CHAN;
	// Set to 0 to use the device's native sample rate.
	config.sampleRate = 44100;
	// This function will be called when miniaudio needs more data.
	config.dataCallback = audio_tx_thread;

	audio_transport_select_dev(&config.capture.pDeviceID, NULL);

	ret = ma_device_init(&s_ma_ctx, &config, &s_spk_src_dev);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init spk_src ma_device: %d\n", ret);
		return -1;
	}

	return 0;
}

void audio_transport_deinit()
{
	ma_device_uninit(&s_spk_src_dev);
	ma_context_uninit(&s_ma_ctx);
}
