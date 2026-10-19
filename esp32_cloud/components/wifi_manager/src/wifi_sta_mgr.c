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
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_config.h"
#include "wifi_sta_mgr.h"
#include "thingsboard_mqtt.h"

static const char *TAG = "wifi_sta";

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
    if (!s_is_connected) {
        ESP_LOGI(TAG, "Retrying Wi-Fi connection: '%s' (Attempt #%lu)...",
                 s_current_ssid, (unsigned long)s_retry_count + 1);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect() failed: %s (0x%x). Rearming retry timer.", esp_err_to_name(err), err);
            esp_timer_start_once(s_reconnect_timer, 2000000ULL);
        }
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
        esp_err_t rec_err = esp_wifi_scan_get_ap_records(&retrieve_count, ap_records);
        if (rec_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get scan records: %s. Clearing scan list.", esp_err_to_name(rec_err));
            esp_wifi_clear_ap_list();
            return;
        }
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

static void sntp_sync_notification_cb(struct timeval *tv) {
    time_t now = tv ? tv->tv_sec : time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "⏰ [SNTP] Time synchronized! Current RTC: %s (epoch=%lld)", strftime_buf, (long long)now);
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

        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;

        /* [L1 FIX] Calculate backoff for current attempt BEFORE incrementing retry count */
        uint32_t backoff_sec = wifi_calculate_backoff_sec(s_retry_count);
        s_retry_count++;
        if (s_retry_count >= CONFIG_WIFI_MAX_RETRY) {
            s_connect_failed = true;
            ESP_LOGW(TAG, "⚠️ Wi-Fi disconnected (Attempt #%lu >= %d threshold). Continuing 30s background retry...",
                     (unsigned long)s_retry_count, CONFIG_WIFI_MAX_RETRY);
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected (Reason: %u, Attempt #%lu). Retrying in %lu seconds...",
                     reason, (unsigned long)s_retry_count, (unsigned long)backoff_sec);
        }

        /* [H05 FIX] Always arm reconnect timer (never give up in 24/7 gateway) */
        if (s_reconnect_timer != NULL && !s_sta_stopped) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)backoff_sec * 1000000ULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* [H6 FIX] If Station was stopped intentionally, ignore queued in-flight GOT_IP events */
        if (s_sta_stopped) {
            ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP received but Station is stopped. Ignoring.");
            return;
        }

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

        /* Resume MQTT client if it was paused during SoftAP provisioning */
        tb_mqtt_resume();

        static bool s_sntp_initialized = false;
        if (!s_sntp_initialized) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 3
            esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
                3,
                ESP_SNTP_SERVER_LIST("time.google.com", "time.cloudflare.com", "pool.ntp.org")
            );
#else
            esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
#endif
            sntp_config.sync_cb = sntp_sync_notification_cb;
            sntp_config.server_from_dhcp = true;
            sntp_config.renew_servers_after_new_IP = true;

            esp_err_t sntp_err = esp_netif_sntp_init(&sntp_config);
            if (sntp_err == ESP_OK) {
                s_sntp_initialized = true;
                ESP_LOGI(TAG, "⏰ [SNTP] Real-time clock synchronization started (time.google.com / time.cloudflare.com / pool.ntp.org)");

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
            } else {
                ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(sntp_err));
            }
        } else {
            esp_netif_sntp_start();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        /* [H8 FIX] L3 IP lease expired or revoked. Mark disconnected immediately */
        ESP_LOGW(TAG, "⚠️ Lost IP address lease (IP_EVENT_STA_LOST_IP). Marking L3 disconnected.");
        s_is_connected = false;
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
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL);
        s_handlers_registered = true;
    }

    /* [H2 FIX] Full-length SSID (32 bytes) and WPA2 PSK (64 hex characters) support per IEEE 802.11 */
    wifi_config_t wifi_config = { 0 };
    size_t ssid_len = strlen(s_current_ssid);
    if (ssid_len > sizeof(wifi_config.sta.ssid)) {
        ssid_len = sizeof(wifi_config.sta.ssid);
    }
    memcpy(wifi_config.sta.ssid, s_current_ssid, ssid_len);

    size_t pass_len = strlen(s_current_pass);
    if (pass_len > sizeof(wifi_config.sta.password)) {
        pass_len = sizeof(wifi_config.sta.password);
    }
    memcpy(wifi_config.sta.password, s_current_pass, pass_len);

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

    /* [H9 FIX] Graceful error checking in runtime without ESP_ERROR_CHECK panic reboots */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Level 1 Power Optimization: Enable minimum modem sleep to shut off RF/PHY between DTIM beacons */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set Wi-Fi power save mode: %s", esp_err_to_name(ps_err));
    }
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);

    /* Limit TX power to 17 dBm (input 68 quantized to 66 in IDF v6.0.2) to prevent close-range distortion */
    esp_wifi_set_max_tx_power(68);

    ESP_LOGI(TAG, "Connecting to Wi-Fi: '%s'...", s_current_ssid);
    /* [H7 FIX] Check initial connect return code */
    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK) {
        ESP_LOGW(TAG, "Initial esp_wifi_connect returned: %s. Reconnect timer will handle retry.", esp_err_to_name(conn_err));
    }

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

/* [M7 FIX] Official ESP-IDF SNTP synchronization status check with calendar epoch guard */
bool wifi_sta_is_time_synced(void) {
    time_t now = 0;
    time(&now);
    if (now > 1700000000) {
        return true;
    }
    return (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
}

esp_err_t wifi_sta_wait_time_sync(uint32_t timeout_ms) {
    if (wifi_sta_is_time_synced()) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK && wifi_sta_is_time_synced()) {
        return ESP_OK;
    }
    return wifi_sta_is_time_synced() ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_get_power_save(int *out_ps_mode) {
    if (out_ps_mode == NULL) return ESP_ERR_INVALID_ARG;
    wifi_ps_type_t ps_type = WIFI_PS_NONE;
    esp_err_t err = esp_wifi_get_ps(&ps_type);
    if (err == ESP_OK) {
        *out_ps_mode = (int)ps_type;
    }
    return err;
}
