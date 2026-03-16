#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "config_store";
static const char *NVS_NAMESPACE = "treadlink";
static const char *NVS_KEY = "config";

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void config_store_defaults(treadlink_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->auto_connect = true;
    config->cadence_factor = 1.4f;
    config->cadence_offset = 120.0f;
    config->wifi_mode = 0; // AP
}

esp_err_t config_store_load(treadlink_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved config, using defaults");
        config_store_defaults(config);
        return ESP_OK;
    }

    size_t len = sizeof(*config);
    err = nvs_get_blob(handle, NVS_KEY, config, &len);
    nvs_close(handle);

    if (err != ESP_OK || len != sizeof(*config)) {
        ESP_LOGW(TAG, "Config read failed or size mismatch, using defaults");
        config_store_defaults(config);
        return ESP_OK;
    }

    // Validate loaded float values
    if (!isfinite(config->cadence_factor) || config->cadence_factor < 0.0f || config->cadence_factor > 10.0f)
        config->cadence_factor = 1.4f;
    if (!isfinite(config->cadence_offset) || config->cadence_offset < 0.0f || config->cadence_offset > 250.0f)
        config->cadence_offset = 120.0f;

    return ESP_OK;
}

esp_err_t config_store_save(const treadlink_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(err));
    }
    return err;
}
