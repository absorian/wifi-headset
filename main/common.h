/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"

#define HOST_SSID_MAX 32
#define HOST_PASS_MAX 32

esp_err_t nvs_storage_set_host_creds(const char *ssid, const char *pass);
esp_err_t nvs_storage_get_host_creds(char *ssid, char *pass);

void wifi_basic_init(void);
void wifi_setup_softap(void);
void wifi_setup_sta(void);

void captive_server_start(void);
void captive_server_stop(void);

ESP_EVENT_DECLARE_BASE(APP_MAIN);
extern esp_event_loop_handle_t g_main_event_loop;

#define APP_TO_CONF_MODE 0
#define APP_TO_MAIN_MODE 1
