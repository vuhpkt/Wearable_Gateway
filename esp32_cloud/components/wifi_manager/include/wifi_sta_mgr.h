#pragma once

/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>

/* FreeRTOS & ESP-IDF System Headers */
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

esp_err_t wifi_sta_init(void);
esp_err_t wifi_sta_init_with_creds(const char *ssid, const char *password);

bool wifi_sta_is_connected(void);
bool wifi_sta_has_failed(void);
void wifi_sta_stop(void);

esp_err_t wifi_sta_wait_connected(TickType_t timeout_ticks);

/* SNTP Real-Time Clock Synchronization */
bool wifi_sta_is_time_synced(void);
esp_err_t wifi_sta_wait_time_sync(uint32_t timeout_ms);

/* Algorithmic Exponential Backoff Calculator (Tested on Host) */
uint32_t wifi_calculate_backoff_sec(uint32_t retry_count);

/* RF Diagnostic Tool: Scan 2.4GHz APs and log RSSI/Channel/SSID */
void wifi_scan_and_log_aps(void);