/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "appNVS";

#define NVS_NSPACE "storage"

#define HOST_SSID_ID "host_ssid"
#define HOST_PASS_ID "host_pass"

esp_err_t nvs_storage_set_host_creds(const char *ssid, const char *pass)
{
	esp_err_t err = ESP_OK;
	nvs_handle_t handle;

	err = nvs_open(NVS_NSPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Error (%s) opening NVS handle!",
			 esp_err_to_name(err));
		return err;
	}

	err = nvs_set_str(handle, HOST_SSID_ID, ssid);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, HOST_SSID_ID ": Failed to write (%s)",
			 esp_err_to_name(err));
		goto end;
	}
	err = nvs_set_str(handle, HOST_PASS_ID, pass);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, HOST_PASS_ID ": Failed to write (%s)",
			 esp_err_to_name(err));
		goto end;
	}

	err = nvs_commit(handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Error (%s) commiting NVS changes!",
			 esp_err_to_name(err));
		goto end;
	}

end:
	nvs_close(handle);
	return err;
}

esp_err_t nvs_storage_get_host_creds(char *ssid, char *pass)
{
	esp_err_t err;
	nvs_handle_t handle;
	size_t required_size = 0;

	err = nvs_open(NVS_NSPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Error (%s) opening NVS handle!",
			 esp_err_to_name(err));
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Reading host creds...");
	required_size = HOST_SSID_MAX;
	err = nvs_get_str(handle, HOST_SSID_ID, ssid, &required_size);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		ssid[0] = '\0';
	} else if (err != ESP_OK) {
		ssid[0] = '\0';
		ESP_LOGE(TAG, HOST_SSID_ID ": Failed to retrieve (%s)",
			 esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, HOST_SSID_ID ": %s", ssid);
	}

	required_size = HOST_PASS_MAX;
	err = nvs_get_str(handle, HOST_PASS_ID, pass, &required_size);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		pass[0] = '\0';
	} else if (err != ESP_OK) {
		pass[0] = '\0';
		ESP_LOGE(TAG, HOST_PASS_ID ": Failed to retrieve (%s)",
			 esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, HOST_PASS_ID ": %s", pass);
	}

	nvs_close(handle);
	return ESP_OK;
}