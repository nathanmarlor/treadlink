#include "ble_ftms_client.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ftms_client";

// FTMS UUIDs
static const ble_uuid16_t FTMS_SVC_UUID = BLE_UUID16_INIT(0x1826);
static const ble_uuid16_t TREADMILL_DATA_CHR_UUID = BLE_UUID16_INIT(0x2ACD);
static const ble_uuid16_t FTMS_CONTROL_POINT_UUID = BLE_UUID16_INIT(0x2AD9);

// FTMS Control Point op codes
#define FTMS_CP_REQUEST_CONTROL   0x00
#define FTMS_CP_RESET             0x01
#define FTMS_CP_SET_TARGET_SPEED  0x02
#define FTMS_CP_SET_TARGET_INCLINE 0x03
#define FTMS_CP_START_RESUME      0x07
#define FTMS_CP_STOP_PAUSE        0x08
#define FTMS_CP_RESPONSE          0x80

// State machine
static volatile ftms_state_t s_state = FTMS_STATE_IDLE;

// Callbacks
static ftms_data_cb_t s_data_cb;
static ftms_conn_cb_t s_conn_cb;
static ftms_scan_cb_t s_scan_cb;

// Connection
static uint16_t s_conn_handle;
static uint16_t s_treadmill_data_val_handle;
static uint16_t s_control_point_val_handle;
static bool s_control_acquired;
static TimerHandle_t s_reconnect_timer;
static TimerHandle_t s_discovery_timer;
static uint8_t s_saved_addr[6];
static uint8_t s_saved_addr_type;
static bool s_should_reconnect;
static uint32_t s_reconnect_delay_ms;
#define RECONNECT_INITIAL_MS  5000
#define RECONNECT_MAX_MS      60000
#define DISCOVERY_TIMEOUT_MS  10000

// Scan results
static ftms_scan_result_t s_scan_results[FTMS_MAX_SCAN_RESULTS];
static uint8_t s_scan_count;

// Forward declarations
static int ftms_gap_event(struct ble_gap_event *event, void *arg);
static int ftms_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);
static int ftms_on_chr_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  const struct ble_gatt_chr *chr, void *arg);
static int ftms_on_svc_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  const struct ble_gatt_svc *service, void *arg);
static int ftms_on_cp_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg);
static int ftms_on_cp_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg);

static void set_state(ftms_state_t new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
        s_state = new_state;
    }
}

// Parse FTMS Treadmill Data characteristic (0x2ACD)
static void parse_treadmill_data(const uint8_t *data, uint16_t len)
{
    if (len < 4) return;

    ftms_treadmill_data_t td = {0};
    uint16_t offset = 0;

    uint16_t flags = data[offset] | (data[offset + 1] << 8);
    offset += 2;

    // Instantaneous Speed is ALWAYS present (0.01 km/h)
    td.speed_001kmh = data[offset] | (data[offset + 1] << 8);
    offset += 2;

    // Average Speed (bit 1)
    if (flags & (1 << 1)) {
        if (offset + 2 > len) return;
        offset += 2;
    }

    // Total Distance (bit 2) - 3 bytes uint24
    if (flags & (1 << 2)) {
        if (offset + 3 > len) return;
        td.total_distance_m = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16);
        td.has_distance = true;
        offset += 3;
    }

    // Inclination + Ramp Angle (bit 3) - sint16 + sint16
    if (flags & (1 << 3)) {
        if (offset + 4 > len) return;
        td.incline_01pct = (int16_t)(data[offset] | (data[offset + 1] << 8));
        td.has_incline = true;
        offset += 4;
    }

    if (s_data_cb) {
        s_data_cb(&td);
    }
}

// GATT notification callback
static int ftms_on_notify(uint16_t conn_handle, uint16_t attr_handle,
                          struct os_mbuf *om, void *arg)
{
    uint16_t len = OS_MBUF_PKTLEN(om);
    uint8_t buf[64];
    if (len > sizeof(buf)) len = sizeof(buf);

    int rc = ble_hs_mbuf_to_flat(om, buf, len, NULL);
    if (rc == 0) {
        parse_treadmill_data(buf, len);
    }
    return 0;
}

// Subscribe to treadmill data notifications
static void subscribe_treadmill_data(uint16_t conn_handle, uint16_t val_handle)
{
    set_state(FTMS_STATE_SUBSCRIBING);
    uint8_t value[2] = {1, 0};
    int rc = ble_gattc_write_flat(conn_handle, val_handle + 1, value, sizeof(value),
                                  ftms_on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Subscribe write failed: %d", rc);
        set_state(FTMS_STATE_DISCOVERING); // stay in discovery state on failure
    }
}

static int ftms_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to treadmill data notifications");
        set_state(FTMS_STATE_STREAMING);
        xTimerStop(s_discovery_timer, 0);

        // Also subscribe to Control Point indications if available
        if (s_control_point_val_handle != 0) {
            uint8_t value[2] = {2, 0}; // 0x0002 = indications
            int rc = ble_gattc_write_flat(conn_handle, s_control_point_val_handle + 1,
                                          value, sizeof(value), ftms_on_cp_subscribe, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "CP indication subscribe failed: %d", rc);
            }
        }
    } else {
        ESP_LOGE(TAG, "Subscribe failed: %d", error->status);
    }
    return 0;
}

static int ftms_on_cp_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to FTMS Control Point indications");
        // Request control of the treadmill
        uint8_t cmd = FTMS_CP_REQUEST_CONTROL;
        int rc = ble_gattc_write_flat(conn_handle, s_control_point_val_handle,
                                      &cmd, 1, ftms_on_cp_write, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Request control write failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "CP indication subscribe failed: %d", error->status);
    }
    return 0;
}

static int ftms_on_cp_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "FTMS Control Point write OK");
    } else {
        ESP_LOGW(TAG, "FTMS Control Point write failed: %d", error->status);
    }
    return 0;
}

// Characteristic discovery callback
static int ftms_on_chr_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        if (s_treadmill_data_val_handle != 0) {
            ESP_LOGI(TAG, "Found Treadmill Data char, handle=%d", s_treadmill_data_val_handle);
            subscribe_treadmill_data(conn_handle, s_treadmill_data_val_handle);
        } else {
            ESP_LOGW(TAG, "Treadmill Data characteristic not found");
        }
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
        return 0;
    }

    if (ble_uuid_cmp(&chr->uuid.u, &TREADMILL_DATA_CHR_UUID.u) == 0) {
        s_treadmill_data_val_handle = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &FTMS_CONTROL_POINT_UUID.u) == 0) {
        s_control_point_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found FTMS Control Point, handle=%d", s_control_point_val_handle);
    }

    return 0;
}

// Service discovery callback
static int ftms_on_svc_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery error: %d", error->status);
        return 0;
    }

    ESP_LOGI(TAG, "FTMS service found, discovering characteristics...");
    int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                      service->end_handle, ftms_on_chr_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Char discovery start failed: %d", rc);
    }

    return 0;
}

// Reconnect task — runs BLE GAP calls on its own stack (not timer service task)
static TaskHandle_t s_reconnect_task_handle;

static void reconnect_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_should_reconnect && s_state == FTMS_STATE_RECONNECTING) {
            ESP_LOGI(TAG, "Attempting reconnection...");
            ftms_client_connect(s_saved_addr, s_saved_addr_type);
        }
    }
}

// Discovery timeout — disconnect if stuck in discovery/subscribe
static void discovery_timeout_cb(TimerHandle_t timer)
{
    if (s_state >= FTMS_STATE_DISCOVERING && s_state <= FTMS_STATE_SUBSCRIBING) {
        ESP_LOGW(TAG, "GATT discovery timed out, disconnecting");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

// Reconnect timer callback — just wakes the reconnect task
static void reconnect_timer_cb(TimerHandle_t timer)
{
    if (s_reconnect_task_handle) {
        xTaskNotifyGive(s_reconnect_task_handle);
    }
}

// GAP event handler
static int ftms_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                          event->disc.length_data);
        if (rc != 0) break;

        bool has_ftms = false;
        for (int i = 0; i < fields.num_uuids16; i++) {
            if (ble_uuid_u16(&fields.uuids16[i].u) == 0x1826) {
                has_ftms = true;
                break;
            }
        }
        if (!has_ftms) break;

        // Check for duplicate — update name/RSSI if found
        bool duplicate = false;
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_results[i].addr, event->disc.addr.val, 6) == 0) {
                duplicate = true;
                if (fields.name != NULL && fields.name_len > 0 &&
                    s_scan_results[i].name[0] == '\0') {
                    size_t copy_len = fields.name_len < sizeof(s_scan_results[i].name) - 1 ?
                                      fields.name_len : sizeof(s_scan_results[i].name) - 1;
                    memcpy(s_scan_results[i].name, fields.name, copy_len);
                    s_scan_results[i].name[copy_len] = '\0';
                }
                if (event->disc.rssi > s_scan_results[i].rssi) {
                    s_scan_results[i].rssi = event->disc.rssi;
                }
                break;
            }
        }
        if (duplicate) break;

        // Add new result
        if (s_scan_count < FTMS_MAX_SCAN_RESULTS) {
            ftms_scan_result_t *r = &s_scan_results[s_scan_count];
            memcpy(r->addr, event->disc.addr.val, 6);
            r->addr_type = event->disc.addr.type;
            r->rssi = event->disc.rssi;

            if (fields.name != NULL && fields.name_len > 0) {
                size_t copy_len = fields.name_len < sizeof(r->name) - 1 ?
                                  fields.name_len : sizeof(r->name) - 1;
                memcpy(r->name, fields.name, copy_len);
                r->name[copy_len] = '\0';
            } else {
                r->name[0] = '\0';
            }

            ESP_LOGI(TAG, "Found FTMS device: %s (RSSI: %d)", r->name, r->rssi);
            s_scan_count++;

            if (s_scan_cb) {
                s_scan_cb(r);
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete, found %d devices", s_scan_count);
        if (s_state == FTMS_STATE_SCANNING) {
            set_state(FTMS_STATE_IDLE);
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            set_state(FTMS_STATE_DISCOVERING);
            s_reconnect_delay_ms = RECONNECT_INITIAL_MS;
            xTimerStart(s_discovery_timer, 0);
            ESP_LOGI(TAG, "Connected to treadmill (handle=%d)", s_conn_handle);

            int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &FTMS_SVC_UUID.u,
                                                  ftms_on_svc_discovered, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Service discovery failed: %d", rc);
            }

            if (s_conn_cb) s_conn_cb(true);
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
            set_state(s_should_reconnect ? FTMS_STATE_RECONNECTING : FTMS_STATE_IDLE);
            if (s_should_reconnect) {
                // Exponential backoff: 5s -> 10s -> 20s -> 40s -> 60s max
                xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_reconnect_delay_ms), 0);
                xTimerStart(s_reconnect_timer, 0);
                ESP_LOGI(TAG, "Reconnect in %lums", (unsigned long)s_reconnect_delay_ms);
                if (s_reconnect_delay_ms < RECONNECT_MAX_MS) {
                    s_reconnect_delay_ms *= 2;
                    if (s_reconnect_delay_ms > RECONNECT_MAX_MS) {
                        s_reconnect_delay_ms = RECONNECT_MAX_MS;
                    }
                }
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected from treadmill (reason=%d)", event->disconnect.reason);
        xTimerStop(s_discovery_timer, 0);
        set_state(s_should_reconnect ? FTMS_STATE_RECONNECTING : FTMS_STATE_IDLE);
        s_treadmill_data_val_handle = 0;
        s_control_point_val_handle = 0;
        s_control_acquired = false;

        if (s_conn_cb) s_conn_cb(false);

        if (s_should_reconnect) {
            // Use current backoff delay (reset on successful connect)
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_reconnect_delay_ms), 0);
            xTimerStart(s_reconnect_timer, 0);
            ESP_LOGI(TAG, "Reconnect in %lums", (unsigned long)s_reconnect_delay_ms);
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_control_point_val_handle) {
            // Control Point indication response
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t resp[8];
            if (len > sizeof(resp)) len = sizeof(resp);
            ble_hs_mbuf_to_flat(event->notify_rx.om, resp, len, NULL);
            if (len >= 3 && resp[0] == FTMS_CP_RESPONSE) {
                uint8_t req_opcode = resp[1];
                uint8_t result = resp[2];
                if (result == 0x01) { // Success
                    ESP_LOGI(TAG, "CP response: op=0x%02X success", req_opcode);
                    if (req_opcode == FTMS_CP_REQUEST_CONTROL) {
                        s_control_acquired = true;
                        ESP_LOGI(TAG, "Treadmill control acquired");
                    }
                } else {
                    ESP_LOGW(TAG, "CP response: op=0x%02X result=0x%02X", req_opcode, result);
                }
            }
        } else {
            ftms_on_notify(event->notify_rx.conn_handle,
                           event->notify_rx.attr_handle,
                           event->notify_rx.om, NULL);
        }
        break;

    default:
        break;
    }

    return 0;
}

esp_err_t ftms_client_init(ftms_data_cb_t data_cb, ftms_conn_cb_t conn_cb)
{
    s_data_cb = data_cb;
    s_conn_cb = conn_cb;
    s_scan_count = 0;
    s_state = FTMS_STATE_IDLE;
    s_should_reconnect = false;
    s_treadmill_data_val_handle = 0;
    s_control_point_val_handle = 0;
    s_control_acquired = false;
    s_reconnect_delay_ms = RECONNECT_INITIAL_MS;

    s_reconnect_timer = xTimerCreate("ftms_recon", pdMS_TO_TICKS(RECONNECT_INITIAL_MS),
                                      pdFALSE, NULL, reconnect_timer_cb);
    s_discovery_timer = xTimerCreate("ftms_disc", pdMS_TO_TICKS(DISCOVERY_TIMEOUT_MS),
                                      pdFALSE, NULL, discovery_timeout_cb);

    xTaskCreate(reconnect_task, "ftms_recon", 3072, NULL, 5, &s_reconnect_task_handle);

    ESP_LOGI(TAG, "FTMS client initialized");
    return ESP_OK;
}

void ftms_client_set_scan_cb(ftms_scan_cb_t cb)
{
    s_scan_cb = cb;
}

esp_err_t ftms_client_scan_start(void)
{
    s_scan_count = 0;
    set_state(FTMS_STATE_SCANNING);

    struct ble_gap_disc_params disc_params = {
        .itvl = 0,
        .window = 0,
        .filter_policy = BLE_HCI_CONN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 10000, &disc_params,
                           ftms_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan start failed: %d", rc);
        set_state(FTMS_STATE_IDLE);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanning for FTMS devices (10s)...");
    return ESP_OK;
}

esp_err_t ftms_client_scan_stop(void)
{
    ble_gap_disc_cancel();
    if (s_state == FTMS_STATE_SCANNING) {
        set_state(FTMS_STATE_IDLE);
    }
    return ESP_OK;
}

esp_err_t ftms_client_connect(const uint8_t addr[6], uint8_t addr_type)
{
    ble_gap_disc_cancel();
    set_state(FTMS_STATE_CONNECTING);

    ble_addr_t peer_addr;
    peer_addr.type = addr_type;
    memcpy(peer_addr.val, addr, 6);

    memcpy(s_saved_addr, addr, 6);
    s_saved_addr_type = addr_type;
    s_should_reconnect = true;

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer_addr, 3000,
                              NULL, ftms_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Connect failed: %d", rc);
        set_state(FTMS_STATE_IDLE);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X...",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return ESP_OK;
}

esp_err_t ftms_client_disconnect(void)
{
    s_should_reconnect = false;
    xTimerStop(s_reconnect_timer, 0);
    xTimerStop(s_discovery_timer, 0);

    if (s_state == FTMS_STATE_CONNECTING) {
        // No conn_handle yet — cancel the pending connect
        ble_gap_conn_cancel();
    } else if (s_state >= FTMS_STATE_DISCOVERING && s_state <= FTMS_STATE_STREAMING) {
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            ESP_LOGE(TAG, "Disconnect failed: %d", rc);
            return ESP_FAIL;
        }
    }
    set_state(FTMS_STATE_IDLE);
    return ESP_OK;
}

bool ftms_client_is_connected(void)
{
    return s_state >= FTMS_STATE_DISCOVERING && s_state <= FTMS_STATE_STREAMING;
}

ftms_state_t ftms_client_get_state(void)
{
    return s_state;
}

const ftms_scan_result_t *ftms_client_get_scan_results(uint8_t *count)
{
    *count = s_scan_count;
    return s_scan_results;
}

void ftms_client_clear_scan_results(void)
{
    s_scan_count = 0;
}

bool ftms_client_has_control(void)
{
    return s_control_acquired;
}

esp_err_t ftms_client_set_target_speed(uint16_t speed_001kmh)
{
    if (!s_control_acquired || s_control_point_val_handle == 0) {
        ESP_LOGW(TAG, "Cannot set speed: no control");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd[3];
    cmd[0] = FTMS_CP_SET_TARGET_SPEED;
    cmd[1] = speed_001kmh & 0xFF;
    cmd[2] = (speed_001kmh >> 8) & 0xFF;

    int rc = ble_gattc_write_flat(s_conn_handle, s_control_point_val_handle,
                                  cmd, sizeof(cmd), ftms_on_cp_write, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Set target speed write failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Set target speed: %d (0.01 km/h)", speed_001kmh);
    return ESP_OK;
}

esp_err_t ftms_client_set_target_incline(int16_t incline_01pct)
{
    if (!s_control_acquired || s_control_point_val_handle == 0) {
        ESP_LOGW(TAG, "Cannot set incline: no control");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd[3];
    cmd[0] = FTMS_CP_SET_TARGET_INCLINE;
    cmd[1] = incline_01pct & 0xFF;
    cmd[2] = (incline_01pct >> 8) & 0xFF;

    int rc = ble_gattc_write_flat(s_conn_handle, s_control_point_val_handle,
                                  cmd, sizeof(cmd), ftms_on_cp_write, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Set target incline write failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Set target incline: %d (0.1%%)", incline_01pct);
    return ESP_OK;
}

esp_err_t ftms_client_start_treadmill(void)
{
    if (!s_control_acquired || s_control_point_val_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t cmd = FTMS_CP_START_RESUME;
    int rc = ble_gattc_write_flat(s_conn_handle, s_control_point_val_handle,
                                  &cmd, 1, ftms_on_cp_write, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ftms_client_stop_treadmill(void)
{
    if (!s_control_acquired || s_control_point_val_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t cmd[2] = {FTMS_CP_STOP_PAUSE, 0x01}; // 0x01 = Stop
    int rc = ble_gattc_write_flat(s_conn_handle, s_control_point_val_handle,
                                  cmd, sizeof(cmd), ftms_on_cp_write, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
