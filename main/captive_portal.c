#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "dns_server.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <sys/param.h>
#include <string.h>

static const char *TAG = "appCAPTIVE";

typedef struct {
    char *page_buf;
    char status_msg[64];
} session_ctx_t;

#define STATUS_NONE " "

extern const char root_start[] asm("_binary_captive_html_start");
extern const char root_end[] asm("_binary_captive_html_end");

static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t login_post_handler(httpd_req_t *req);

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler
};

static const httpd_uri_t login_uri = {
    .uri = "/login",
    .method = HTTP_POST,
    .handler = login_post_handler
};

static esp_err_t nvs_storage_set_host_creds(const char *ssid, const char *pass)
{
    static SemaphoreHandle_t mutex = NULL;
    esp_err_t err = ESP_OK;
    nvs_handle_t handle;

    if (mutex == NULL)
        mutex = xSemaphoreCreateMutex();

    // For consistency of ssid and pass pairs configuration
    xSemaphoreTake(mutex, portMAX_DELAY);

    err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, "host_ssid", ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write ssid!");
        return err;
    }
    err = nvs_set_str(handle, "host_pass", pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write pass!");
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes!");
        return err;
    }

    nvs_close(handle);
    xSemaphoreGive(mutex);
    return err;
}

#ifdef CONFIG_ESP_ENABLE_DHCP_CAPTIVEPORTAL
void dhcp_set_captiveportal_url(void) {
    // get the IP of the access point to redirect to
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

    // turn the IP into a URI
    char* captiveportal_uri = (char*) malloc(32 * sizeof(char));
    assert(captiveportal_uri && "Failed to allocate captiveportal_uri");
    strcpy(captiveportal_uri, "http://");
    strcat(captiveportal_uri, ip_addr);

    // get a handle to configure DHCP with
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    // set the DHCP option 114
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captiveportal_uri, strlen(captiveportal_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}
#endif // CONFIG_ESP_ENABLE_DHCP_CAPTIVEPORTAL

static void session_ctx_free(void *ctx) {
    free(((session_ctx_t *)ctx)->page_buf);
    free(ctx);
}

static session_ctx_t *session_ctx_get(httpd_req_t *req)
{
    if (!req->sess_ctx) {
        req->sess_ctx = malloc(sizeof(session_ctx_t));
        session_ctx_t *ctx = req->sess_ctx;

        ctx->page_buf = malloc(root_end - root_start);
        strcpy(ctx->status_msg, STATUS_NONE);
        
        // Set free callback
        req->free_ctx = session_ctx_free;
    }

    return req->sess_ctx;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint32_t root_len = root_end - root_start;
    session_ctx_t *ctx = session_ctx_get(req);

    ESP_LOGI(TAG, "Serve root");
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);

    snprintf(ctx->page_buf, root_len, root_start, ctx->status_msg);
    httpd_resp_send(req, ctx->page_buf, root_len);

    return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    session_ctx_t *ctx = session_ctx_get(req);
    ESP_LOGI(TAG, "Login received");

    char buf[128];
    int ret;
    esp_err_t err;

    // Read the POST body data
    if (req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
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

    // 2. Extract specific keys from the buffer
    char ssid[32], pass[32];
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

    ESP_LOGI(TAG, "Login attempt: ssid=%s, pass=%s", ssid, pass);
    err = nvs_storage_set_host_creds(ssid, pass);
    if (err != ESP_OK) {
        strcpy(ctx->status_msg, "Internal error");
        goto end;
    }

    strcpy(ctx->status_msg, "Success");

end:
    httpd_resp_set_status(req, "303");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}



// HTTP Error (404) Handler - Redirects all requests to the root page
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "303 See Other");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &login_uri);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&dns_config);
}
