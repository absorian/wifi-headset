/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "esp_check.h"
#include "esp_sleep.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "freertos/portmacro.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "appMAIN";

ESP_EVENT_DEFINE_BASE(APP_MAIN);
esp_event_loop_handle_t g_main_event_loop;

enum app_mode {
	APP_MODE_SLEEP = 0,
	APP_MODE_CONF,
	APP_MODE_MAIN
} s_app_mode;

static enum app_mode app_power_switch()
{
	if (s_app_mode == APP_MODE_SLEEP)
		return APP_MODE_MAIN;
	return APP_MODE_SLEEP;
}

static enum app_mode app_conf_switch()
{
	if (s_app_mode == APP_MODE_SLEEP)
		return s_app_mode;

	if (s_app_mode == APP_MODE_MAIN)
		return APP_MODE_CONF;
	return APP_MODE_MAIN;
}

static void main_event_handler(void *handler_arg, esp_event_base_t base,
			       int32_t id, void *event_data)
{
	if (strcmp(base, APP_MAIN))
		return;

	switch (id) {
	case APP_CONF_SWITCH:
	case APP_POWER_SWITCH:
		enum app_mode to = id == APP_CONF_SWITCH ?
						   app_conf_switch() : app_power_switch();
		if (s_app_mode == to)
			break;
    	ESP_LOGI(TAG, "Mode switch %d -> %d", s_app_mode, to);
		vTaskDelay(pdMS_TO_TICKS(500));
		// deinit
		switch (s_app_mode) {
			case APP_MODE_CONF:
				captive_server_stop();
				break;
			case APP_MODE_MAIN:
				audio_transport_stop();
				control_transport_stop();
				i2s_periph_deinit();
				discovery_stop();
				break;
			case APP_MODE_SLEEP:
				esp_netif_init();
				esp_event_loop_create_default();
				wifi_handle_init();
				break;
		}
		// init
		s_app_mode = to;
		switch (s_app_mode) {
			case APP_MODE_CONF:
				wifi_handle_setup_softap();
				captive_server_start();
				break;
			case APP_MODE_MAIN:
				wifi_handle_setup_sta();
				i2s_periph_init();
				break;
			case APP_MODE_SLEEP:
				wifi_handle_deinit();
				esp_event_loop_delete_default();
				esp_netif_deinit();

				esp_deep_sleep_start();
				break;
		}
		break;
	case APP_WIFI_CONNECTED:
    	ESP_LOGI(TAG, "WIFI connected");
		discovery_start();
		break;
	case APP_HOST_FOUND:
		discovery_stop();
		host_conn_info_t *info = event_data;
    	ESP_LOGI(TAG, "Host found ip=%s udp=%hu tcp=%hu",
				 info->ip, info->udp_port, info->tcp_port);
		control_transport_start(info);
		audio_transport_setup(info);
		break;
	case APP_HOST_CONNECTED:
    	ESP_LOGI(TAG, "Host connected");
		discovery_stop();
		audio_transport_start();
		break;
	case APP_HOST_DISCONNECTED:
    	ESP_LOGI(TAG, "Host diconnected");
		audio_transport_stop();
		control_transport_stop();
		discovery_start();
		break;

	default:
		break;
	}
}

void app_main(void)
{
	esp_err_t err;
	esp_event_loop_args_t event_loop_args = { .queue_size = 8,
						  .task_name = NULL };

	ESP_ERROR_CHECK(
		esp_event_loop_create(&event_loop_args, &g_main_event_loop));
	ESP_ERROR_CHECK(esp_event_handler_register_with(
		g_main_event_loop, APP_MAIN, ESP_EVENT_ANY_ID,
		main_event_handler, NULL));

	periph_init();

	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	while (1) {
		esp_event_loop_run(g_main_event_loop, pdMS_TO_TICKS(10));
	}

	esp_event_loop_delete(g_main_event_loop);
}