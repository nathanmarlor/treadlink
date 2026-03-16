#include "web_server.h"
#include "config_store.h"
#include "data_bridge.h"
#include "ble_ftms_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "web_server";

#include "index_html.h"

// --- Log ring buffer ---
#define LOG_MAX_ENTRIES 50
#define LOG_MAX_LEN 128

typedef struct {
    uint32_t id;
    char level;
    char text[LOG_MAX_LEN];
} log_entry_t;

static log_entry_t s_log_ring[LOG_MAX_ENTRIES];
static uint32_t s_log_write_idx;
static uint32_t s_log_next_id = 1;

// Simulate callback
static web_simulate_cb_t s_simulate_cb;

// Shared state protected by mutex
static SemaphoreHandle_t s_mutex;
static struct {
    float speed_kmh;
    uint8_t cadence_spm;
    uint32_t distance_m;
    float incline_pct;
    bool treadmill_connected;
    bool garmin_connected;
    char treadmill_name[32];
    char treadmill_addr[18];
} s_live_data;

// --- Log API ---

void web_log(char level, const char *fmt, ...)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    log_entry_t *e = &s_log_ring[s_log_write_idx % LOG_MAX_ENTRIES];
    e->id = s_log_next_id++;
    e->level = level;

    va_list args;
    va_start(args, fmt);
    vsnprintf(e->text, LOG_MAX_LEN, fmt, args);
    va_end(args);

    s_log_write_idx++;
    xSemaphoreGive(s_mutex);
}

// --- Handlers ---

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req)
{
    char buf[384];
    ftms_state_t ftms_state = ftms_client_get_state();
    bool reconnecting = (ftms_state == FTMS_STATE_RECONNECTING);
    uint16_t recon_attempts = ftms_client_get_reconnect_attempts();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int len = snprintf(buf, sizeof(buf),
        "{\"treadmill_connected\":%s,\"garmin_connected\":%s,"
        "\"speed_kmh\":%.2f,\"cadence_spm\":%u,\"distance_m\":%lu,"
        "\"incline_pct\":%.1f,\"speed_is_mph\":%s,"
        "\"treadmill_name\":\"%s\",\"treadmill_addr\":\"%s\","
        "\"has_control\":%s,\"reconnecting\":%s,\"reconnect_attempts\":%u}",
        s_live_data.treadmill_connected ? "true" : "false",
        s_live_data.garmin_connected ? "true" : "false",
        (double)s_live_data.speed_kmh,
        (unsigned)s_live_data.cadence_spm,
        (unsigned long)s_live_data.distance_m,
        (double)s_live_data.incline_pct,
        data_bridge_is_mph_mode() ? "true" : "false",
        s_live_data.treadmill_name,
        s_live_data.treadmill_addr,
        ftms_client_has_control() ? "true" : "false",
        reconnecting ? "true" : "false",
        (unsigned)recon_attempts);
    xSemaphoreGive(s_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static void json_escape(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_sz; i++) {
        char c = src[i];
        if (c == '"')       { dst[j++] = '\\'; dst[j++] = '"'; }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else                { dst[j++] = c; }
    }
    dst[j] = '\0';
}

static esp_err_t handler_log(httpd_req_t *req)
{
    uint32_t after_id = 0;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
            after_id = (uint32_t)atoi(val);
        }
    }

    // Max per entry: {"id":99999,"level":"W","text":"..."} ≈ 180 bytes
    // 50 entries * 180 + brackets + commas ≈ 9200 bytes
    char *buf = malloc(LOG_MAX_ENTRIES * 180 + 64);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
        return ESP_OK;
    }

    int pos = 0;
    buf[pos++] = '[';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t start = s_log_write_idx < LOG_MAX_ENTRIES ? 0 : s_log_write_idx - LOG_MAX_ENTRIES;
    bool first = true;
    for (uint32_t i = start; i < s_log_write_idx; i++) {
        const log_entry_t *e = &s_log_ring[i % LOG_MAX_ENTRIES];
        if (e->id > after_id) {
            char escaped[LOG_MAX_LEN * 2];
            json_escape(escaped, sizeof(escaped), e->text);
            pos += snprintf(buf + pos, LOG_MAX_ENTRIES * 180 + 64 - pos,
                "%s{\"id\":%lu,\"level\":\"%c\",\"text\":\"%s\"}",
                first ? "" : ",",
                (unsigned long)e->id, e->level, escaped);
            first = false;
        }
    }
    xSemaphoreGive(s_mutex);

    buf[pos++] = ']';
    buf[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

static esp_err_t handler_scan(httpd_req_t *req)
{
    ftms_client_clear_scan_results();
    esp_err_t err = ftms_client_scan_start();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        web_log('I', "Scanning for FTMS treadmills...");
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
    } else {
        web_log('E', "Scan start failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"scan_failed\"}");
    }
    return ESP_OK;
}

static esp_err_t handler_scan_results(httpd_req_t *req)
{
    uint8_t count = 0;
    const ftms_scan_result_t *results = ftms_client_get_scan_results(&count);

    // ~100 bytes per entry, max ~10 results
    char buf[1200];
    int pos = 0;
    buf[pos++] = '[';

    for (int i = 0; i < count; i++) {
        char name_esc[64];
        json_escape(name_esc, sizeof(name_esc), results[i].name);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"addr_type\":%u,\"name\":\"%s\",\"rssi\":%d}",
            i ? "," : "",
            results[i].addr[5], results[i].addr[4], results[i].addr[3],
            results[i].addr[2], results[i].addr[1], results[i].addr[0],
            (unsigned)results[i].addr_type, name_esc, results[i].rssi);
    }

    buf[pos++] = ']';
    buf[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

static bool parse_addr(const char *str, uint8_t addr[6])
{
    unsigned int a[6];
    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        addr[i] = (uint8_t)a[5 - i];
    }
    return true;
}

static esp_err_t handler_connect(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"no_body\"}");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    const cJSON *addr_item = cJSON_GetObjectItem(json, "addr");
    const cJSON *type_item = cJSON_GetObjectItem(json, "addr_type");

    if (!cJSON_IsString(addr_item)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"missing_addr\"}");
        return ESP_OK;
    }

    uint8_t addr[6];
    if (!parse_addr(addr_item->valuestring, addr)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"bad_addr\"}");
        return ESP_OK;
    }

    uint8_t addr_type = cJSON_IsNumber(type_item) ? (uint8_t)type_item->valuedouble : 0;
    const cJSON *name_item = cJSON_GetObjectItem(json, "name");
    char treadmill_name[32] = {0};
    if (cJSON_IsString(name_item) && name_item->valuestring[0] != '\0') {
        strncpy(treadmill_name, name_item->valuestring, sizeof(treadmill_name) - 1);
    }
    cJSON_Delete(json);

    // Save treadmill address + name for auto-connect and UI display
    {
        treadlink_config_t cfg;
        config_store_load(&cfg);
        snprintf(cfg.treadmill_addr, sizeof(cfg.treadmill_addr),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        cfg.treadmill_addr_type = addr_type;
        strncpy(cfg.treadmill_name, treadmill_name, sizeof(cfg.treadmill_name) - 1);
        config_store_save(&cfg);

        // Update cached display info
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        strncpy(s_live_data.treadmill_name, treadmill_name, sizeof(s_live_data.treadmill_name) - 1);
        strncpy(s_live_data.treadmill_addr, cfg.treadmill_addr, sizeof(s_live_data.treadmill_addr) - 1);
        xSemaphoreGive(s_mutex);
    }

    web_log('I', "Connecting to %s...", treadmill_name[0] ? treadmill_name : "treadmill");
    esp_err_t err = ftms_client_connect(addr, addr_type);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"connecting\"}");
    } else {
        web_log('E', "Connect request failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"connect_failed\"}");
    }
    return ESP_OK;
}

static esp_err_t handler_disconnect(httpd_req_t *req)
{
    ftms_client_disconnect();
    web_log('I', "Disconnecting from treadmill");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"disconnecting\"}");
    return ESP_OK;
}

static esp_err_t handler_config_get(httpd_req_t *req)
{
    treadlink_config_t config;
    config_store_load(&config);

    char name_esc[64], ssid_esc[68];
    json_escape(name_esc, sizeof(name_esc), config.treadmill_name);
    json_escape(ssid_esc, sizeof(ssid_esc), config.wifi_ssid);

    char buf[320];
    int len = snprintf(buf, sizeof(buf),
        "{\"treadmill_addr\":\"%s\",\"treadmill_name\":\"%s\","
        "\"auto_connect\":%s,\"cadence_factor\":%.2f,"
        "\"cadence_offset\":%.1f,\"speed_is_mph\":%s,"
        "\"wifi_mode\":%u,\"wifi_ssid\":\"%s\"}",
        config.treadmill_addr, name_esc,
        config.auto_connect ? "true" : "false",
        (double)config.cadence_factor,
        (double)config.cadence_offset,
        config.speed_is_mph ? "true" : "false",
        (unsigned)config.wifi_mode, ssid_esc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handler_config_post(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"no_body\"}");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    // Single NVS load — use as both old (for diff) and base (for merge)
    treadlink_config_t config;
    config_store_load(&config);

    // Snapshot WiFi fields before merge for change detection
    uint8_t old_wifi_mode = config.wifi_mode;
    char old_ssid[33], old_pass[65];
    strncpy(old_ssid, config.wifi_ssid, sizeof(old_ssid));
    strncpy(old_pass, config.wifi_pass, sizeof(old_pass));

    const cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "cadence_factor")) && cJSON_IsNumber(item))
        config.cadence_factor = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "cadence_offset")) && cJSON_IsNumber(item))
        config.cadence_offset = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "wifi_mode")) && cJSON_IsNumber(item))
        config.wifi_mode = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "wifi_ssid")) && cJSON_IsString(item))
        strncpy(config.wifi_ssid, item->valuestring, sizeof(config.wifi_ssid) - 1);
    if ((item = cJSON_GetObjectItem(json, "wifi_pass")) && cJSON_IsString(item) && strlen(item->valuestring) > 0)
        strncpy(config.wifi_pass, item->valuestring, sizeof(config.wifi_pass) - 1);
    if ((item = cJSON_GetObjectItem(json, "auto_connect")) && cJSON_IsBool(item))
        config.auto_connect = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "speed_is_mph")) && cJSON_IsBool(item))
        config.speed_is_mph = cJSON_IsTrue(item);

    cJSON_Delete(json);

    // Validate config bounds
    if (config.cadence_factor < 0.0f || config.cadence_factor > 10.0f) config.cadence_factor = 1.4f;
    if (config.cadence_offset < 0.0f || config.cadence_offset > 250.0f) config.cadence_offset = 120.0f;

    bool wifi_changed = (old_wifi_mode != config.wifi_mode ||
                         strcmp(old_ssid, config.wifi_ssid) != 0 ||
                         strcmp(old_pass, config.wifi_pass) != 0);

    esp_err_t err = config_store_save(&config);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        data_bridge_update_config(&config);

        if (wifi_changed) {
            web_log('I', "WiFi config changed, restarting...");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr(req, "{\"status\":\"saved\",\"restart\":true}");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        } else {
            web_log('I', "Config saved");
            httpd_resp_sendstr(req, "{\"status\":\"saved\"}");
        }
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"save_failed\"}");
    }
    return ESP_OK;
}

// --- Treadmill Control ---

static esp_err_t handler_control(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"no_body\"}");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    if (!ftms_client_has_control()) {
        cJSON_Delete(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"no_control\",\"msg\":\"Treadmill control not available\"}");
        return ESP_OK;
    }

    const cJSON *action = cJSON_GetObjectItem(json, "action");
    esp_err_t err = ESP_OK;

    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "set_speed") == 0) {
            const cJSON *val = cJSON_GetObjectItem(json, "speed_kmh");
            if (cJSON_IsNumber(val)) {
                double clamped = val->valuedouble < 0 ? 0 : (val->valuedouble > 25.0 ? 25.0 : val->valuedouble);
                uint16_t speed = (uint16_t)(clamped * 100.0);
                err = ftms_client_set_target_speed(speed);
            }
        } else if (strcmp(action->valuestring, "set_incline") == 0) {
            const cJSON *val = cJSON_GetObjectItem(json, "incline_pct");
            if (cJSON_IsNumber(val)) {
                double clamped = val->valuedouble < -5.0 ? -5.0 : (val->valuedouble > 15.0 ? 15.0 : val->valuedouble);
                int16_t incline = (int16_t)(clamped * 10.0);
                err = ftms_client_set_target_incline(incline);
            }
        } else if (strcmp(action->valuestring, "start") == 0) {
            err = ftms_client_start_treadmill();
            web_log('I', "Treadmill start");
        } else if (strcmp(action->valuestring, "stop") == 0) {
            err = ftms_client_stop_treadmill();
            web_log('I', "Treadmill stop");
        }
    }

    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_sendstr(req, "{\"error\":\"command_failed\"}");
    }
    return ESP_OK;
}

static esp_err_t handler_control_status(httpd_req_t *req)
{
    char buf[64];
    int len = snprintf(buf, sizeof(buf),
        "{\"has_control\":%s,\"connected\":%s}",
        ftms_client_has_control() ? "true" : "false",
        ftms_client_is_connected() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// --- Simulate ---

void web_server_set_simulate_callback(web_simulate_cb_t cb)
{
    s_simulate_cb = cb;
}

static esp_err_t handler_simulate(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"no_body\"}");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    if (!s_simulate_cb) {
        cJSON_Delete(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"no_callback\"}");
        return ESP_OK;
    }

    const cJSON *speed_item = cJSON_GetObjectItem(json, "speed_kmh");
    const cJSON *incline_item = cJSON_GetObjectItem(json, "incline_pct");

    ftms_treadmill_data_t ftms = {0};
    if (cJSON_IsNumber(speed_item)) {
        ftms.speed_001kmh = (uint16_t)(speed_item->valuedouble * 100.0);
    }
    if (cJSON_IsNumber(incline_item)) {
        ftms.incline_01pct = (int16_t)(incline_item->valuedouble * 10.0);
        ftms.has_incline = true;
    }

    cJSON_Delete(json);
    s_simulate_cb(&ftms);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// --- URI registration ---

static const httpd_uri_t uri_index = {
    .uri = "/", .method = HTTP_GET, .handler = handler_index
};
static const httpd_uri_t uri_status = {
    .uri = "/api/status", .method = HTTP_GET, .handler = handler_status
};
static const httpd_uri_t uri_log = {
    .uri = "/api/log", .method = HTTP_GET, .handler = handler_log
};
static const httpd_uri_t uri_scan = {
    .uri = "/api/scan", .method = HTTP_POST, .handler = handler_scan
};
static const httpd_uri_t uri_scan_results = {
    .uri = "/api/scan/results", .method = HTTP_GET, .handler = handler_scan_results
};
static const httpd_uri_t uri_connect = {
    .uri = "/api/connect", .method = HTTP_POST, .handler = handler_connect
};
static const httpd_uri_t uri_disconnect = {
    .uri = "/api/disconnect", .method = HTTP_POST, .handler = handler_disconnect
};
static const httpd_uri_t uri_config_get = {
    .uri = "/api/config", .method = HTTP_GET, .handler = handler_config_get
};
static const httpd_uri_t uri_config_post = {
    .uri = "/api/config", .method = HTTP_POST, .handler = handler_config_post
};
static const httpd_uri_t uri_control = {
    .uri = "/api/control", .method = HTTP_POST, .handler = handler_control
};
static const httpd_uri_t uri_simulate = {
    .uri = "/api/simulate", .method = HTTP_POST, .handler = handler_simulate
};
static const httpd_uri_t uri_control_status = {
    .uri = "/api/control/status", .method = HTTP_GET, .handler = handler_control_status
};

esp_err_t web_server_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_live_data, 0, sizeof(s_live_data));
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_write_idx = 0;
    s_log_next_id = 1;

    // Pre-load cached treadmill info from NVS
    treadlink_config_t cfg;
    config_store_load(&cfg);
    strncpy(s_live_data.treadmill_name, cfg.treadmill_name, sizeof(s_live_data.treadmill_name) - 1);
    strncpy(s_live_data.treadmill_addr, cfg.treadmill_addr, sizeof(s_live_data.treadmill_addr) - 1);

    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_scan_results);
    httpd_register_uri_handler(server, &uri_connect);
    httpd_register_uri_handler(server, &uri_disconnect);
    httpd_register_uri_handler(server, &uri_config_get);
    httpd_register_uri_handler(server, &uri_config_post);
    httpd_register_uri_handler(server, &uri_control);
    httpd_register_uri_handler(server, &uri_control_status);
    httpd_register_uri_handler(server, &uri_simulate);

    ESP_LOGI(TAG, "Web server started");
    web_log('I', "TreadLink started");
    return ESP_OK;
}

void web_server_update_data(const ftms_treadmill_data_t *ftms, const rsc_data_t *rsc,
                            bool treadmill_connected, bool garmin_connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_live_data.speed_kmh = data_bridge_get_display_speed_kmh();
    s_live_data.cadence_spm = rsc->cadence_spm;
    s_live_data.distance_m = rsc->total_distance_dm / 10;
    s_live_data.incline_pct = ftms->has_incline ? ftms->incline_01pct / 10.0f : 0.0f;
    s_live_data.treadmill_connected = treadmill_connected;
    s_live_data.garmin_connected = garmin_connected;
    xSemaphoreGive(s_mutex);
}

void web_server_set_connection_status(bool treadmill_connected, bool garmin_connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_live_data.treadmill_connected = treadmill_connected;
    s_live_data.garmin_connected = garmin_connected;
    xSemaphoreGive(s_mutex);
}
