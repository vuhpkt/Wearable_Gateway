#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_portal_new_creds_cb_t)(const char *ssid, const char *password);

/**
 * @brief Start the embedded HTTP web server for Wi-Fi provisioning.
 *
 * Serves GET / with configuration form and handles POST /save.
 *
 * @param on_creds_received Callback invoked when valid SSID/Password is submitted.
 * @return ESP_OK on success, or error code.
 */
esp_err_t wifi_portal_start(wifi_portal_new_creds_cb_t on_creds_received);

/**
 * @brief Stop the HTTP web server cleanly.
 */
void wifi_portal_stop(void);

#ifdef __cplusplus
}
#endif
