/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "dns_server.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

#include <sys/param.h>
#include <string.h>

static const char *TAG = "appCAPTIVE";

typedef struct {
	char *page_buf;
	char status_msg[64];
} session_ctx_t;

#define STATUS_NONE " "

extern const char s_root_start[] asm("_binary_captive_html_start");
extern const char s_root_end[] asm("_binary_captive_html_end");

static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t login_post_handler(httpd_req_t *req);
static esp_err_t restart_post_handler(httpd_req_t *req);

static const httpd_uri_t s_root_uri = { .uri = "/",
					.method = HTTP_GET,
					.handler = root_get_handler };

static const httpd_uri_t s_login_uri = { .uri = "/login",
					 .method = HTTP_POST,
					 .handler = login_post_handler };

static const httpd_uri_t s_restart_uri = { .uri = "/restart",
					   .method = HTTP_POST,
					   .handler = restart_post_handler };

static httpd_handle_t s_server_handle = NULL;
static dns_server_handle_t s_dns_handle = NULL;

static void percent_decode(char *s)
{
	char *d = s; // Destination pointer
	while (*s) {
		if (*s == '%' && isxdigit((uint8_t)s[1]) &&
		    isxdigit((uint8_t)s[2])) {
			// Helper to convert hex to integer
			int hi = isdigit((uint8_t)s[1]) ?
					 s[1] - '0' :
					 toupper((uint8_t)s[1]) - 'A' + 10;
			int lo = isdigit((uint8_t)s[2]) ?
					 s[2] - '0' :
					 toupper((uint8_t)s[2]) - 'A' + 10;
			*d++ = (char)((hi << 4) | lo);
			s += 3;
		} else if (*s == '+') {
			// In many URL contexts, '+' represents a space
			*d++ = ' ';
			s++;
		} else {
			*d++ = *s++;
		}
	}
	*d = '\0';
}

#ifdef CONFIG_ESP_ENABLE_DHCP_CAPTIVEPORTAL
static void dhcp_set_captiveportal_url(void)
{
	static char *captiveportal_uri = NULL;
	esp_netif_ip_info_t ip_info;
	esp_netif_t *netif;

	// get the IP of the access point to redirect to
	esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
			      &ip_info);

	char ip_addr[16];
	inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
	ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

	// turn the IP into a URI
	if (captiveportal_uri == NULL)
		captiveportal_uri = (char *)malloc(32 * sizeof(char));
	assert(captiveportal_uri && "Failed to allocate captiveportal_uri");
	strcpy(captiveportal_uri, "http://");
	strcat(captiveportal_uri, ip_addr);

	// get a handle to configure DHCP with
	netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

	// set the DHCP option 114
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
	ESP_ERROR_CHECK(esp_netif_dhcps_option(
		netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
		captiveportal_uri, strlen(captiveportal_uri)));
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}
#endif // CONFIG_ESP_ENABLE_DHCP_CAPTIVEPORTAL

static void session_ctx_free(void *ctx)
{
	free(((session_ctx_t *)ctx)->page_buf);
	free(ctx);
}

static session_ctx_t *session_ctx_get(httpd_req_t *req)
{
	session_ctx_t *ctx;

	if (!req->sess_ctx) {
		req->sess_ctx = malloc(sizeof(session_ctx_t));
		ctx = req->sess_ctx;

		ctx->page_buf = malloc(s_root_end - s_root_start +
				       sizeof(ctx->status_msg));
		strcpy(ctx->status_msg, STATUS_NONE);

		// Set free callback
		req->free_ctx = session_ctx_free;
	}

	return req->sess_ctx;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
	const uint32_t root_len = s_root_end - s_root_start;
	session_ctx_t *ctx = session_ctx_get(req);

	ESP_LOGI(TAG, "Serve root");
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);

	snprintf(ctx->page_buf, root_len, s_root_start, ctx->status_msg);
	httpd_resp_send(req, ctx->page_buf, root_len);
	strcpy(ctx->status_msg, STATUS_NONE);

	return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
	session_ctx_t *ctx = session_ctx_get(req);
	char buf[128];
	int ret;
	esp_err_t err;

	ESP_LOGI(TAG, "Login received");

	// Read the POST body data
	if (req->content_len >= sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
				    "Content too long");
		return ESP_FAIL;
	}

	ret = httpd_req_recv(req, buf, req->content_len);
	if (ret <= 0) { // Handle error or timeout
		if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
			httpd_resp_send_408(req);
		}
		return ESP_FAIL;
	}
	buf[ret] = '\0';

	// Extract specific keys from the buffer
	char ssid[HOST_SSID_MAX], pass[HOST_PASS_MAX];
	ret = httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
	if (ret != ESP_OK || !strlen(ssid)) {
		strcpy(ctx->status_msg, "Invalid SSID");
		goto end;
	}
	ret = httpd_query_key_value(buf, "pass", pass, sizeof(pass));
	if (ret != ESP_OK || !strlen(pass)) {
		strcpy(ctx->status_msg, "Invalid password");
		goto end;
	}

	percent_decode(ssid);
	percent_decode(pass);

	ESP_LOGI(TAG, "Login attempt: ssid=%s, pass=%s", ssid, pass);
	err = nvs_storage_set_host_creds(ssid, pass);
	if (err != ESP_OK) {
		strcpy(ctx->status_msg, "Internal error");
		goto end;
	}

	strcpy(ctx->status_msg, "Changes saved");

end:
	httpd_resp_set_status(req, "303");
	httpd_resp_set_hdr(req, "Location", "/");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

static esp_err_t restart_post_handler(httpd_req_t *req)
{
	session_ctx_t *ctx = session_ctx_get(req);
	esp_err_t err;

	ESP_LOGI(TAG, "Restart requested");

	err = esp_event_post_to(g_main_event_loop, APP_MAIN, APP_TO_MAIN_MODE,
				NULL, 0, portMAX_DELAY);
	if (err != ESP_OK) {
		strcpy(ctx->status_msg, "Internal Error, please try again");
	} else {
		strcpy(ctx->status_msg, "Device is restarting...");
	}

	httpd_resp_set_status(req, "303");
	httpd_resp_set_hdr(req, "Location", "/");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

// HTTP Error (404) Handler - Redirects all requests to the root page
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
	// Set status
	httpd_resp_set_status(req, "303 See Other");
	// Redirect to the "/" root directory
	httpd_resp_set_hdr(req, "Location", "/");
	// iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
	httpd_resp_send(req, "Redirect to the captive portal",
			HTTPD_RESP_USE_STRLEN);

	return ESP_OK;
}

void captive_server_start(void)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE(
		"*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);

#ifdef CONFIG_ESP_ENABLE_DHCP_CAPTIVEPORTAL
	dhcp_set_captiveportal_url();
#endif

	config.max_open_sockets = 7;
	config.lru_purge_enable = true;

	// Start the httpd server
	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if (httpd_start(&s_server_handle, &config) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start server");
		return;
	}

	ESP_LOGI(TAG, "Registering URI handlers");
	httpd_register_uri_handler(s_server_handle, &s_root_uri);
	httpd_register_uri_handler(s_server_handle, &s_login_uri);
	httpd_register_uri_handler(s_server_handle, &s_restart_uri);
	httpd_register_err_handler(s_server_handle, HTTPD_404_NOT_FOUND,
				   http_404_error_handler);

	// Start the DNS server that will redirect all queries to the softAP IP
	s_dns_handle = start_dns_server(&dns_config);
}

void captive_server_stop(void)
{
	ESP_LOGI(TAG, "Stopping server");
	httpd_stop(s_server_handle);
	stop_dns_server(s_dns_handle);
}
