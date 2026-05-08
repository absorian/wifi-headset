/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "esp_check.h"
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

static bool s_is_conf_mode = false;

static void main_event_handler(void *handler_arg, esp_event_base_t base,
			       int32_t id, void *event_data)
{
	if (strcmp(base, APP_MAIN))
		return;

	switch (id) {
	case APP_TO_MAIN_MODE:
		if (!s_is_conf_mode)
			break;
		// Let caller finalize its business
		vTaskDelay(pdMS_TO_TICKS(500));
		s_is_conf_mode = false;

		captive_server_stop();
		wifi_setup_sta();
		// enable i2s periphs
		break;
	case APP_TO_CONF_MODE:
		if (s_is_conf_mode)
			break;
		// Let caller finalize its business
		vTaskDelay(pdMS_TO_TICKS(500));
		s_is_conf_mode = true;

		wifi_setup_softap();
		captive_server_start();
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

	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(
		esp_event_loop_create(&event_loop_args, &g_main_event_loop));
	ESP_ERROR_CHECK(esp_event_handler_register_with(
		g_main_event_loop, APP_MAIN, ESP_EVENT_ANY_ID,
		main_event_handler, NULL));

	ESP_ERROR_CHECK(esp_netif_init());
	wifi_basic_init();

	s_is_conf_mode = true;
	wifi_setup_softap();
	captive_server_start();

	while (1) {
		esp_event_loop_run(g_main_event_loop, pdMS_TO_TICKS(10));
	}

	esp_event_loop_delete(g_main_event_loop);
}