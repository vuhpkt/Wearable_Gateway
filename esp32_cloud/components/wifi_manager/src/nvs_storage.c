/* Standard C Libraries */
#include <string.h>

/* ESP-IDF System Headers */
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

/* Project Local Headers */
#include "nvs_storage.h"

static const char *TAG = "nvs_storage";

static const char *NVS_NAMESPACE = "wifi_store";
static const char *KEY_SSID = "ssid";
static const char *KEY_PASS = "pass";

void nvs_storage_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated or new version found. Erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

esp_err_t nvs_storage_read_wifi(char *ssid, size_t ssid_size, char *password, size_t pass_size) {
    if (!ssid || !password)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Namespace '%s' does not exist in NVS yet", NVS_NAMESPACE);
        return err;
    }

    size_t req_ssid_len = ssid_size;
    err = nvs_get_str(handle, KEY_SSID, ssid, &req_ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    size_t req_pass_len = pass_size;
    err = nvs_get_str(handle, KEY_PASS, password, &req_pass_len);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_wifi(const char *ssid, const char *password) {
    if (!ssid || !password)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_SSID, ssid);
    if (err == ESP_OK) nvs_set_str(handle, KEY_PASS, password);
    if (err == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "New Wi-Fi credentials saved to NVS: SSID='%s'", ssid);
    } else {
        ESP_LOGE(TAG, "Failed to write NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
};