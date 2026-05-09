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

static const char *TAG = "appCTRL";

static int s_sock = -1;
static TaskHandle_t s_task = NULL;

static void control_transport_task(void *param)
{
    int ret;
    struct sockaddr_in *addr = param;

    ret = connect(s_sock, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        ESP_LOGE(TAG, "Socket connect fail: errno %d", errno);
        vTaskSuspend(NULL);
    }

    esp_event_post_to(g_main_event_loop, APP_MAIN, APP_HOST_CONNECTED,
                      NULL, 0, portMAX_DELAY);

    while (1) {
        ret = recv(s_sock, NULL, 0, MSG_DONTWAIT | MSG_PEEK);

        if (ret == 0) {
            // disconnected
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_event_post_to(g_main_event_loop, APP_MAIN, APP_HOST_DISCONNECTED,
                      NULL, 0, portMAX_DELAY);
    vTaskSuspend(NULL);
}

void control_transport_start(const host_conn_info_t *info)
{
    static struct sockaddr_in addr;
    int ret;

    if (s_task != NULL) return;

    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_sock < 0) {
		ESP_LOGE(TAG, "Socket create fail: errno %d", errno);
		return;
	}

    addr.sin_addr.s_addr = inet_addr(info->ip);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(info->tcp_port);

	xTaskCreate(control_transport_task, "control_transport", 2048, &addr, 0, &s_task);
}

void control_transport_stop()
{
    if (s_task == NULL) return;

    close(s_sock);

    vTaskDelete(s_task);
	s_task = NULL;
}
