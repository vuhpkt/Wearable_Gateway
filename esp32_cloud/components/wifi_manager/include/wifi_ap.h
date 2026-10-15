#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_ap_timeout_cb_t)(void);

/**
 * @brief Configure idle timeout for SoftAP provisioning.
 *
 * @param timeout_sec Timeout in seconds (default: 300 = 5 minutes). 0 to disable.
 * @param cb Callback invoked when timeout expires without credentials being submitted.
 */
void wifi_ap_set_timeout(uint32_t timeout_sec, wifi_ap_timeout_cb_t cb);

/**
 * @brief Start SoftAP mode on ESP32-S3.
 *
 * Configures DHCP server (192.168.4.1), starts AP interface.
 *
 * @param ssid AP SSID (e.g. "ESP32_HEALTH_PROV")
 * @param password AP Password (NULL or "" for Open)
 * @return ESP_OK on success, or error code.
 */
esp_err_t wifi_ap_start(const char *ssid, const char *password);

/**
 * @brief Stop SoftAP mode cleanly.
 */
void wifi_ap_stop(void);

/**
 * @brief Check if SoftAP is currently active.
 */
bool wifi_ap_is_active(void);

#ifdef __cplusplus
}
#endif
