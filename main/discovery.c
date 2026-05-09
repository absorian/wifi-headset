/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "appDISC";

// TODO: make configurable via portal
#define SV_DISCOVERY_PORT 48672
#define CL_NAME "My WIFI Headset" // [ a-zA-Z0-9_\-]
#define CL_UID 0xbead97cfde8f7c9a

#define CL_NAME_MAX 32
#define RESEND_INTV_SEC 3

static int s_bcast_sock;
static TaskHandle_t s_task = NULL;

static void discovery_task(void *param)
{
	int ret;
	struct sockaddr_in source_addr;
	struct sockaddr_in dest_addr;
	socklen_t socklen;
	char payload[128];
	char rx_buf[128];
	host_conn_info_t host_info;

	sprintf(payload, "hdst_conn_req,uid='%llx',name='" CL_NAME "'", CL_UID);

	dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(SV_DISCOVERY_PORT);

	while (1) {
		ret = sendto(s_bcast_sock, payload, strlen(payload) + 1, 0,
			     (struct sockaddr *)&dest_addr, sizeof(dest_addr));
		if (ret < 0) {
			ESP_LOGE(TAG, "sendto fail: errno %d", errno);
			break;
		}

		socklen = sizeof(source_addr);
		ret = recvfrom(s_bcast_sock, rx_buf, sizeof(rx_buf), 0,
			       (struct sockaddr *)&source_addr, &socklen);
		if (ret < 0) {
			if (errno != EAGAIN)
				ESP_LOGE(TAG, "recvfrom fail: errno %d", errno);
			continue;
		}

		if (ntohs(source_addr.sin_port) != SV_DISCOVERY_PORT) {
			continue;
		}

		rx_buf[sizeof(rx_buf) - 1] = '\0';
		ret = sscanf(rx_buf, "hdst_conn_resp,udp=%hu,tcp=%hu",
			     &host_info.udp_port, &host_info.tcp_port);
		if (ret != 2) {
			ESP_LOGE(TAG, "Received invalid response");
			continue;
		}

		strcpy(host_info.ip, inet_ntoa(source_addr.sin_addr));
		ESP_LOGI(TAG, "Found host on %s with ports udp=%u, tcp=%u",
			 host_info.ip, host_info.udp_port, host_info.tcp_port);
		esp_event_post_to(g_main_event_loop, APP_MAIN, APP_HOST_FOUND,
				  &host_info, sizeof(host_info), portMAX_DELAY);
		break;
	}
	// Task will be deleted externally
	vTaskSuspend(NULL);
}

void discovery_start(void)
{
	const int broadcast_en = 1;
	struct timeval timeout;
	int ret;

	if (s_task != NULL)
		return;

	ESP_LOGI(TAG, "Preparing for broadcast");

	s_bcast_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (s_bcast_sock < 0) {
		ESP_LOGE(TAG, "Socket create fail: errno %d", errno);
		return;
	}

	// Set timeout
	timeout.tv_sec = RESEND_INTV_SEC;
	timeout.tv_usec = 0;
	ret = setsockopt(s_bcast_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			 sizeof timeout);
	if (ret < 0) {
		ESP_LOGE(TAG, "setsockopt SO_RCVTIMEO failed");
	}

	ret = setsockopt(s_bcast_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_en,
			 sizeof(broadcast_en));
	if (ret < 0) {
		ESP_LOGE(TAG, "setsockopt SO_BROADCAST failed");
	}

	xTaskCreate(discovery_task, "conn_handle", 2048, NULL, 0, &s_task);
}

void discovery_stop(void)
{
	if (s_task == NULL)
		return;

	ESP_LOGI(TAG, "Stop/deinit broadcast");

	vTaskDelete(s_task);
	s_task = NULL;

	close(s_bcast_sock);
}
