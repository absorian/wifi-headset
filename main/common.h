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

typedef struct {
	char ip[16];
	uint16_t udp_port;
	uint16_t tcp_port;
} host_conn_info_t;

void discovery_start(void);
void discovery_stop(void);

void control_transport_start(const host_conn_info_t *info);
void control_transport_stop();

ESP_EVENT_DECLARE_BASE(APP_MAIN);
extern esp_event_loop_handle_t g_main_event_loop;

enum { APP_TO_CONF_MODE, APP_TO_MAIN_MODE, 
	APP_WIFI_CONNECTED, 
	APP_HOST_FOUND, APP_HOST_CONNECTED, APP_HOST_DISCONNECTED };
