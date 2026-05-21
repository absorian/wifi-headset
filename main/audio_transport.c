/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "driver/i2s_std.h"

#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <errno.h>

#define AUDIO_BUF_SIZE 512

typedef struct {
	uint16_t seqnum;
} audio_hdr_t;

extern i2s_chan_handle_t g_i2s_tx;
extern i2s_chan_handle_t g_i2s_rx;

static const char *TAG = "appAUDIO";

static int s_socket = -1;
static TaskHandle_t s_rx_task = NULL;

static void audio_rx_task(void *param)
{
	int ret;
	uint16_t last_seqnum = 0;

	uint8_t payload[AUDIO_BUF_SIZE + sizeof(audio_hdr_t)];
	audio_hdr_t *hdr = (audio_hdr_t *)payload;
	uint8_t *buf = payload + sizeof(audio_hdr_t);
	int buf_sz = 0;
	int stat_total = 0, stat_lost = 0;

	ESP_LOGI(TAG, "RX starting to listen");

	while (1) {
		ret = recv(s_socket, &payload, sizeof(payload), 0);
		if (ret < 0) {
			ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
			continue;
		}
		stat_total++;

		if (last_seqnum == 0 || last_seqnum + 1 == hdr->seqnum) {
			buf_sz = ret - sizeof(audio_hdr_t);
			i2s_channel_write(g_i2s_tx, buf, buf_sz, NULL,
					  portMAX_DELAY);
		} else {
			stat_lost++;
		}
		last_seqnum = hdr->seqnum;

		if (stat_total % 1000 == 0) {
			ESP_LOGI(TAG, "RX stat total=%d lost=%d", stat_total,
				 stat_lost);
		}
	}
}

void audio_transport_setup(const host_conn_info_t *info)
{
	int ret;
	struct sockaddr_in addr;

	if (s_socket >= 0)
		return;

	s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (s_socket < 0) {
		ESP_LOGE(TAG, "Socket create fail: errno %d", errno);
		return;
	}

	addr.sin_addr.s_addr = IPADDR_ANY;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(info->udp_port);
	ret = bind(s_socket, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		ESP_LOGE(TAG, "Bind fail: errno %d", errno);
		close(s_socket);
		return;
	}

	addr.sin_addr.s_addr = inet_addr(info->ip);
	ret = connect(s_socket, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		ESP_LOGE(TAG, "Connect fail: errno %d", errno);
		close(s_socket);
		return;
	}
}

void audio_transport_start(void)
{
	if (s_socket < 0 || s_rx_task != NULL)
		return;

	// create rx task
	xTaskCreate(audio_rx_task, "audio_rx", 4096, NULL, 2, &s_rx_task);
}

void audio_transport_stop(void)
{
	if (s_socket < 0)
		return;

	vTaskDelete(s_rx_task);
	s_rx_task = NULL;

	close(s_socket);
	s_socket = -1;
}
