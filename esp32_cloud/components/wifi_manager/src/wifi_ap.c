/*
 * SoftAP Manager Driver
 * ESP-IDF v6.0.2 / ESP32-S3
 */

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "wifi_ap.h"

static const char *TAG = "WIFI_AP";

static esp_netif_t *s_netif_ap = NULL;
static bool s_ap_active = false;
static bool s_handlers_registered = false;
static esp_timer_handle_t s_ap_timeout_timer = NULL;
static wifi_ap_timeout_cb_t s_timeout_cb = NULL;
static uint32_t s_timeout_sec = 300; /* Default: 5 minutes */

static void on_ap_timeout_timer(void *arg) {
    (void)arg;
    if (s_ap_active) {
        ESP_LOGW(TAG, "⏰ [TIMEOUT] SoftAP idle for %lu seconds without configuration! Auto-stopping...", (unsigned long)s_timeout_sec);
        if (s_timeout_cb != NULL) {
            s_timeout_cb();
        } else {
            wifi_ap_stop();
        }
    }
}

void wifi_ap_set_timeout(uint32_t timeout_sec, wifi_ap_timeout_cb_t cb) {
    s_timeout_sec = timeout_sec;
    s_timeout_cb = cb;
}

static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "📱 Client connected to SoftAP! MAC=%02x:%02x:%02x:%02x:%02x:%02x, AID=%d",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "📱 Client disconnected from SoftAP. MAC=%02x:%02x:%02x:%02x:%02x:%02x, AID=%d",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
    }
}

esp_err_t wifi_ap_start(const char *ssid, const char *password) {
    if (!ssid) return ESP_ERR_INVALID_ARG;

    if (!s_netif_ap) {
        s_netif_ap = esp_netif_create_default_wifi_ap();
    }

    if (!s_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &wifi_ap_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &wifi_ap_event_handler, NULL, NULL));
        s_handlers_registered = true;
    }

    wifi_config_t wifi_ap_config = {0};
    strncpy((char *)wifi_ap_config.ap.ssid, ssid, sizeof(wifi_ap_config.ap.ssid) - 1);
    wifi_ap_config.ap.ssid_len = strlen(ssid);
    wifi_ap_config.ap.channel = 1;
    wifi_ap_config.ap.max_connection = 4;

    if (password && strlen(password) >= 8) {
        strncpy((char *)wifi_ap_config.ap.password, password, sizeof(wifi_ap_config.ap.password) - 1);
        wifi_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_active = true;

    /* Arm SoftAP Idle Timeout timer to prevent endless RF radiation and heating */
    if (s_timeout_sec > 0) {
        if (s_ap_timeout_timer == NULL) {
            esp_timer_create_args_t timer_args = {
                .callback = on_ap_timeout_timer,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "softap_timeout_tmr"
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ap_timeout_timer));
        }
        esp_timer_stop(s_ap_timeout_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_ap_timeout_timer, (uint64_t)s_timeout_sec * 1000000ULL));
        ESP_LOGI(TAG, "⏱ SoftAP Idle Timeout armed: %lu seconds (auto-shutdown if inactive)", (unsigned long)s_timeout_sec);
    }

    ESP_LOGI(TAG, "===============================================================");
    ESP_LOGI(TAG, ">>> SoftAP STARTED SUCCESSFULLY! <<<");
    ESP_LOGI(TAG, "    SSID: %s", ssid);
    ESP_LOGI(TAG, "    IP:   192.168.4.1 (Web Server ready)");
    ESP_LOGI(TAG, "===============================================================");
    return ESP_OK;
}

void wifi_ap_stop(void) {
    if (s_ap_timeout_timer != NULL) {
        esp_timer_stop(s_ap_timeout_timer);
    }
    if (s_ap_active) {
        esp_wifi_stop();
        s_ap_active = false;
        ESP_LOGI(TAG, "SoftAP stopped.");
    }
}

bool wifi_ap_is_active(void) {
    return s_ap_active;
}
