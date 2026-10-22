/*
 * Industrial-Grade Wi-Fi Station Manager for Wearable Gateway
 * Target SoC: ESP32-S3 (ESP-IDF v6.0.2)
 *
 * Architecture & Design Standards (See docs/decisions/ADR-001.md):
 * 1. Dynamic 802.11 active channel discovery (channel = 0, Channels 1..13).
 * 2. Universal PMF compatibility (capable = true, required = false).
 * 3. TX power limit: 17 dBm (68 * 0.25 dBm) to prevent close-range LNA saturation.
 * 4. Progressive Cooldown Exponential Backoff (2s -> 5s -> 10s -> 20s -> 30s).
 * 5. Zero recursive disconnects: no esp_wifi_disconnect() inside disconnected callback.
 * 6. Zero heap fragmentation: in-RAM dynamic transition support without esp_restart().
 * 7. SNTP real-time clock synchronization for ThingsBoard timestamps.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_config.h"
#include "wifi_sta_mgr.h"

static const char *TAG = "wifi_sta";

static wifi_sta_got_ip_cb_t s_got_ip_cb = NULL;

void wifi_sta_set_got_ip_cb(wifi_sta_got_ip_cb_t cb) {
    s_got_ip_cb = cb;
}

static esp_netif_t *s_netif_sta = NULL;
static volatile bool s_is_connected = false;
static volatile bool s_connect_failed = false;
static volatile bool s_has_connected_once = false;
static volatile bool s_sta_stopped = false;
static uint32_t s_retry_count = 0;
static char s_current_ssid[33] = {0};
static char s_current_pass[65] = {0};
static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_sntp_resync_timer = NULL;
static bool s_handlers_registered = false;
static wifi_sta_sntp_cb_t s_sntp_cb = NULL;

void wifi_sta_set_sntp_cb(wifi_sta_sntp_cb_t cb) {
    s_sntp_cb = cb;
}

static void sntp_sync_notification_cb(struct timeval *tv) {
    if (tv) {
        ESP_LOGI(TAG, "⏰ [SNTP] Real-time synchronized: sec=%lld, usec=%ld",
                 (long long)tv->tv_sec, (long)tv->tv_usec);
        if (s_sntp_cb != NULL) {
            s_sntp_cb(tv);
        }
    }
}

uint32_t wifi_calculate_backoff_sec(uint32_t retry_count) {
    if (retry_count == 0) return 2;
    if (retry_count == 1) return 5;
    if (retry_count == 2) return 10;
    if (retry_count == 3) return 20;
    return 30;
}

static void on_reconnect_timer(void *arg) {
    (void)arg;
    if (s_sta_stopped) {
        return;
    }
    if (!s_is_connected && !s_connect_failed) {
        ESP_LOGI(TAG, "Retrying Wi-Fi connection: '%s' (Attempt #%lu)...",
                 s_current_ssid, (unsigned long)s_retry_count + 1);
        esp_wifi_connect();
    }
}

static const char *wifi_auth_mode_to_str(wifi_auth_mode_t authmode) {
    switch (authmode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK (SAE)";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3_PSK (Mixed)";
        case WIFI_AUTH_OWE: return "OWE";
        default: return "UNKNOWN";
    }
}

void wifi_scan_and_log_aps(void) {
    ESP_LOGI(TAG, "📡 Scanning 2.4GHz Wi-Fi channels 1-13...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };
    esp_err_t scan_err = esp_wifi_scan_start(&scan_config, true);
    if (scan_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s (0x%x)", esp_err_to_name(scan_err), scan_err);
        return;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 0) {
        wifi_ap_record_t ap_records[8]; /* Stack allocation: auto-freed, no .bss overhead */
        uint16_t retrieve_count = (ap_count > 8) ? 8 : ap_count;
        esp_wifi_scan_get_ap_records(&retrieve_count, ap_records);
        ESP_LOGI(TAG, "🔍 [SCAN RESULTS] Found %u 2.4GHz Wi-Fi APs (displaying %u):", ap_count, retrieve_count);
        bool found_target = false;
        for (int i = 0; i < retrieve_count; i++) {
            bool is_target = (strlen(s_current_ssid) > 0 && strcmp((char *)ap_records[i].ssid, s_current_ssid) == 0);
            if (is_target) found_target = true;
            ESP_LOGI(TAG, "   [%02d] %sSSID: '%s' | Channel: %u | RSSI: %d dBm | Auth: %s (%u)%s",
                     i + 1,
                     is_target ? "★ [TARGET MATCH] " : "",
                     (char *)ap_records[i].ssid,
                     ap_records[i].primary,
                     ap_records[i].rssi,
                     wifi_auth_mode_to_str(ap_records[i].authmode),
                     ap_records[i].authmode,
                     is_target ? " ★" : "");
        }
        if (strlen(s_current_ssid) > 0) {
            if (found_target) {
                ESP_LOGI(TAG, "🎯 TARGET AP '%s' FOUND IN 2.4GHz SCAN LIST!", s_current_ssid);
            } else {
                ESP_LOGW(TAG, "⚠️ TARGET AP '%s' NOT FOUND in %u scanned APs!", s_current_ssid, ap_count);
            }
        }
    } else {
        ESP_LOGW(TAG, "🔍 [SCAN RESULTS] Zero 2.4GHz APs found!");
    }
}

static void on_sntp_resync_timer(void *arg) {
    (void)arg;
    if (s_is_connected) {
        ESP_LOGI(TAG, "⏰ [SNTP] Triggering periodic real-time resync...");
        esp_netif_sntp_start();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA hardware started.");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;

        /* If Station was stopped intentionally (e.g. user pressed BOOT for SoftAP), do NOT retry */
        if (s_sta_stopped) {
            ESP_LOGI(TAG, "Wi-Fi Station disconnected (Station stopped as intended).");
            if (s_reconnect_timer != NULL) {
                esp_timer_stop(s_reconnect_timer);
            }
            return;
        }

        s_retry_count++;
        /* 
         * IMPORTANT:
         * Do NOT invoke esp_wifi_disconnect() here.
         * Driver v6.0.2 is already in disconnected state; invoking disconnect triggers Reason 36.
         */
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;

        uint32_t backoff_sec = wifi_calculate_backoff_sec(s_retry_count);
        ESP_LOGW(TAG, "Wi-Fi disconnected (Reason: %u, Attempt #%lu). Retrying in %lu seconds...",
                 reason, (unsigned long)s_retry_count, (unsigned long)backoff_sec);

        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)backoff_sec * 1000000ULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "===============================================================");
        ESP_LOGI(TAG, ">>> WI-FI CONNECTED & IP ASSIGNED SUCCESSFULLY! <<<");
        ESP_LOGI(TAG, "    SSID:    %s", s_current_ssid);
        ESP_LOGI(TAG, "    IP:      " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "    Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "    Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "===============================================================");

        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
        }
        s_retry_count = 0;
        s_connect_failed = false;
        s_is_connected = true;
        s_has_connected_once = true;

        /* Notify registered callback (e.g. to resume MQTT client) */
        if (s_got_ip_cb != NULL) {
            s_got_ip_cb();
        }

        static bool s_sntp_initialized = false;
        if (!s_sntp_initialized) {
            esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_config.sync_cb = sntp_sync_notification_cb;
            esp_netif_sntp_init(&sntp_config);
            s_sntp_initialized = true;
            ESP_LOGI(TAG, "⏰ [SNTP] Real-time clock synchronization started via pool.ntp.org");

            if (s_sntp_resync_timer == NULL) {
                esp_timer_create_args_t tmr_args = {
                    .callback = on_sntp_resync_timer,
                    .arg = NULL,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "sntp_resync_tmr"
                };
                if (esp_timer_create(&tmr_args, &s_sntp_resync_timer) == ESP_OK) {
                    esp_timer_start_periodic(s_sntp_resync_timer, 3600ULL * 1000000ULL);
                }
            }
        }
    }
}

esp_err_t wifi_sta_init_with_creds(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_is_connected = false;
    s_connect_failed = false;
    s_sta_stopped = false;
    s_retry_count = 0;

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
    if (password) {
        strncpy(s_current_pass, password, sizeof(s_current_pass) - 1);
        s_current_pass[sizeof(s_current_pass) - 1] = '\0';
    } else {
        s_current_pass[0] = '\0';
    }

    if (!s_netif_sta) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
    }

    if (s_reconnect_timer == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = on_reconnect_timer,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_reconnect_tmr"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    if (!s_handlers_registered) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
        s_handlers_registered = true;
    }

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, s_current_ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (strlen(s_current_pass) > 0) {
        strncpy((char *)wifi_config.sta.password, s_current_pass, sizeof(wifi_config.sta.password) - 1);
    }

    /* 
     * IEEE 802.11 Dynamic Channel Discovery:
     * channel = 0: Quét toàn dải Kênh 1..13 (hỗ trợ mọi router và hotspot)
     */
    wifi_config.sta.channel = 0;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    /* 
     * PMF Compatibility:
     * capable = true, required = false: Hỗ trợ WPA3/PMF Hotspot (Windows 11) mà vẫn chạy được WPA2 cũ
     */
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    wifi_country_t country = {
        .cc = "VN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Level 1 Power Optimization: Enable minimum modem sleep to shut off RF/PHY between DTIM beacons */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);

    /* Limit TX power to 17 dBm (68 * 0.25 dBm) to prevent close-range LNA saturation (<0.5m) */
    esp_wifi_set_max_tx_power(68);

    ESP_LOGI(TAG, "Connecting to Wi-Fi: '%s'...", s_current_ssid);
    esp_wifi_connect();

    return ESP_OK;
}

esp_err_t wifi_sta_init(void) {
    return wifi_sta_init_with_creds(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
}

bool wifi_sta_is_connected(void) {
    return s_is_connected;
}

bool wifi_sta_has_failed(void) {
    return s_connect_failed;
}

void wifi_sta_stop(void) {
    s_sta_stopped = true;
    s_is_connected = false;
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi Station stopped.");
}

esp_err_t wifi_sta_wait_connected(TickType_t timeout_ticks) {
    TickType_t start = xTaskGetTickCount();
    while (!s_is_connected) {
        if (s_sta_stopped) {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_connect_failed) {
            return ESP_FAIL;
        }
        if (timeout_ticks != portMAX_DELAY && (xTaskGetTickCount() - start) > timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

bool wifi_sta_is_time_synced(void) {
    time_t now = 0;
    time(&now);
    return (now > 1700000000);
}

esp_err_t wifi_sta_wait_time_sync(uint32_t timeout_ms) {
    if (wifi_sta_is_time_synced()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (s_sntp_cb != NULL) {
            s_sntp_cb(&tv);
        }
        return ESP_OK;
    }
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK && wifi_sta_is_time_synced()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (s_sntp_cb != NULL) {
            s_sntp_cb(&tv);
        }
        return ESP_OK;
    }
    return wifi_sta_is_time_synced() ? ESP_OK : ESP_ERR_TIMEOUT;
}
