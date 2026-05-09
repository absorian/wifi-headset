/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define WIFI_STA_MAX_RETRIES 5
#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_STA_FAIL_BIT BIT1

static const char *TAG = "appWIFI";

static EventGroupHandle_t s_wifi_event_group;
static int s_sta_retry_num = 0;

static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
			       int32_t event_id, void *event_data)
{
	esp_err_t err;

	if (event_id == WIFI_EVENT_AP_STACONNECTED) {
		wifi_event_ap_staconnected_t *event =
			(wifi_event_ap_staconnected_t *)event_data;
		ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
			 MAC2STR(event->mac), event->aid);
	} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
		wifi_event_ap_stadisconnected_t *event =
			(wifi_event_ap_stadisconnected_t *)event_data;
		ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d, reason=%d",
			 MAC2STR(event->mac), event->aid, event->reason);
	} else if (event_base == WIFI_EVENT &&
		   event_id == WIFI_EVENT_STA_START) {
		ESP_LOGI(TAG, "Connecting to the AP");
		err = esp_wifi_connect();
		if (err == ESP_ERR_WIFI_SSID) {
			s_sta_retry_num = 999;
		}
	} else if (event_base == WIFI_EVENT &&
		   event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_sta_retry_num < WIFI_STA_MAX_RETRIES) {
			esp_wifi_connect();
			s_sta_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		} else {
			xEventGroupSetBits(s_wifi_event_group,
					   WIFI_STA_FAIL_BIT);
		}
		ESP_LOGI(TAG, "connect to the AP fail");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_sta_retry_num = 0;
		esp_event_post_to(g_main_event_loop, APP_MAIN,
				  APP_WIFI_CONNECTED, NULL, 0, portMAX_DELAY);
		xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
	}
}

void wifi_basic_init(void)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	s_netif_ap = esp_netif_create_default_wifi_ap();
	s_netif_sta = esp_netif_create_default_wifi_sta();

	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
		NULL));
}

void wifi_setup_softap(void)
{
	esp_netif_set_default_netif(s_netif_ap);

	wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .ssid_len = strlen(CONFIG_ESP_WIFI_SSID),
            .channel = 1,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .max_connection = CONFIG_ESP_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
#if CONFIG_ESP_GTK_REKEYING_ENABLE
            .gtk_rekey_interval = CONFIG_ESP_GTK_REKEY_INTERVAL,
#else
            .gtk_rekey_interval = 0,
#endif
        },
    };
	if (strlen(CONFIG_ESP_WIFI_PASSWORD) == 0) {
		wifi_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	esp_wifi_stop();
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s",
		 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
}

void wifi_setup_sta(void)
{
	char ssid[HOST_SSID_MAX], pass[HOST_PASS_MAX];
	esp_err_t err;

	esp_netif_set_default_netif(s_netif_sta);

	wifi_config_t wifi_sta_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = WIFI_STA_MAX_RETRIES,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
            .disable_wpa3_compatible_mode = 0,
#endif
        },
    };

	err = nvs_storage_get_host_creds(ssid, pass);
	if (err == ESP_OK) {
		strcpy((char *)wifi_sta_config.sta.ssid, ssid);
		strcpy((char *)wifi_sta_config.sta.password, pass);
	}

	esp_wifi_stop();
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
	ESP_ERROR_CHECK(esp_wifi_start());
}
