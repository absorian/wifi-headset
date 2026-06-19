/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"

#include <stdbool.h>

#define HOST_SSID_MAX 32
#define HOST_PASS_MAX 32

esp_err_t nvs_storage_set_host_creds(const char *ssid, const char *pass);
esp_err_t nvs_storage_get_host_creds(char *ssid, char *pass);

void wifi_handle_init(void);
void wifi_handle_deinit(void);
void wifi_handle_setup_softap(void);
void wifi_handle_setup_sta(void);

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

void i2s_periph_start(void);
void i2s_periph_stop(void);
void i2s_periph_open(const host_conn_info_t *info);
void i2s_periph_close(void);
void i2s_periph_init(void);
void i2s_periph_deinit(void);

enum periph_status_led_mode {
	STATUS_LED_EN,
	STATUS_LED_OFF,
	STATUS_LED_BLINK,
	STATUS_LED_FLASH,
};

void periph_init();
bool periph_did_wakeup();
void periph_deinit();
void periph_status_led_mode_set(enum periph_status_led_mode mode);

ESP_EVENT_DECLARE_BASE(APP_MAIN);
extern esp_event_loop_handle_t g_main_event_loop;

enum {
	APP_POWER_SWITCH,
	APP_CONF_SWITCH,
	APP_WIFI_CONNECTED,
	APP_HOST_FOUND,
	APP_HOST_CONNECTED,
	APP_HOST_DISCONNECTED
};
