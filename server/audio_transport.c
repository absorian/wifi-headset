/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"
#include <miniaudio.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_BUF_SIZE 512

#define AUDIO_FRAME_SIZE (2 * 2)

typedef struct {
	uint16_t seqnum;
} audio_hdr_t;

static int s_socket = -1;
static ma_context s_ma_ctx;
static ma_device s_spk_src_dev;
static ma_device s_mic_dest_dev;

typedef struct {
	uint8_t payload[AUDIO_BUF_SIZE + sizeof(audio_hdr_t)];
	audio_hdr_t *hdr;
	uint8_t *buf;
} audio_tx_ctx_t;

static void audio_tx_thread(ma_device *pDevice, void *pOutput,
			    const void *pInput, ma_uint32 frameCount)
{
	audio_tx_ctx_t *ctx = (audio_tx_ctx_t *)pDevice->pUserData;
	int ret;

	memcpy(ctx->buf, pInput, frameCount * AUDIO_FRAME_SIZE);
	ret = send(s_socket, ctx->payload, sizeof(ctx->payload), 0);
	ctx->hdr->seqnum++;
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
	int ret;
	struct sockaddr_in addr;
	socklen_t socklen;

	s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (s_socket < 0) {
		fprintf(stderr, "Failed to open socket errno=%d\n", errno);
		return -1;
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(0);
	ret = bind(s_socket, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Failed to bind socket errno=%d\n", errno);
		close(s_socket);
		s_socket = -1;
		return -1;
	}

	socklen = sizeof(addr);
	ret = getsockname(s_socket, (struct sockaddr *)&addr, &socklen);
	if (ret < 0) {
		fprintf(stderr, "Failed to getsockname errno=%d\n", errno);
		close(s_socket);
		s_socket = -1;
		return -1;
	}

	// Set the selected port to send back to cl
	conn_info->audio_port = ntohs(addr.sin_port);

	// Connect to cl on the same port
	addr.sin_addr = conn_info->addr.sin_addr;
	ret = connect(s_socket, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Failed to connect errno=%d\n", errno);
		close(s_socket);
		s_socket = -1;
		return -1;
	}

	return 0;
}

void audio_transport_close_conn()
{
	close(s_socket);
}

int audio_transport_init()
{
	audio_tx_ctx_t *ctx;
	ma_result ret;
	ma_device_config config = ma_device_config_init(ma_device_type_capture);
	ret = ma_context_init(NULL, 0, NULL, &s_ma_ctx);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init ma_context: %d\n", ret);
		return -1;
	}

	ctx = malloc(sizeof(audio_tx_ctx_t));
	if (ctx == NULL) {
		fprintf(stderr, "Failed to malloc\n");
		return -1;
	}

	ctx->hdr = (audio_hdr_t *)ctx->payload;
	ctx->buf = ctx->payload + sizeof(audio_hdr_t);

	config.periodSizeInFrames = AUDIO_BUF_SIZE / AUDIO_FRAME_SIZE;
	// Set to 0 to use the device's native channel count.
	config.capture.format = ma_format_s16;
	// Set to 0 to use the device's native channel count.
	config.capture.channels = 2;
	// Set to 0 to use the device's native sample rate.
	config.sampleRate = 44100;
	// This function will be called when miniaudio needs more data.
	config.dataCallback = audio_tx_thread;
	// Can be accessed from the device object (device.pUserData).
	config.pUserData = ctx;

	audio_transport_select_dev(&config.capture.pDeviceID, NULL);

	ret = ma_device_init(&s_ma_ctx, &config, &s_spk_src_dev);
	if (ret != MA_SUCCESS) {
		fprintf(stderr, "Failed to init spk_src ma_device: %d\n", ret);
		free(ctx);
		return -1;
	}

	return 0;
}

void audio_transport_deinit()
{
	free(s_spk_src_dev.pUserData);

	ma_device_uninit(&s_spk_src_dev);
	ma_context_uninit(&s_ma_ctx);
}