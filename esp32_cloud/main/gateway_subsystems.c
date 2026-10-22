#include "gateway_subsystems.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/time.h>
static const char *TAG = "gateway_subsystems";
#else
#include <sys/time.h>
static const char *TAG __attribute__((unused)) = "gateway_subsystems";
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#endif
#ifndef ESP_LOGE
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#endif
#endif

static void on_wifi_ip_assigned(void) {
    ESP_LOGI(TAG, "Wi-Fi IP acquired -> resuming MQTT client");
    tb_mqtt_resume();
}

static void on_sntp_sync(struct timeval *tv) {
    if (!tv) return;
    int64_t now_utc_ms = (int64_t)tv->tv_sec * 1000LL + (int64_t)(tv->tv_usec / 1000);
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    uint64_t esp_mono_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
#else
    uint64_t esp_mono_ms = 0;
#endif
    uart_bridge_post_sntp(now_utc_ms, esp_mono_ms);
}

esp_err_t gateway_subsystems_init(QueueHandle_t control_queue) {
    /* 1. Initialize Ingest & Bridge Subsystem (Spinlocks, Mailbox, Time, Queue, Rev Table)
     *    Must run first so that SNTP and Ingest callbacks find initialized data structures. */
    esp_err_t err = uart_bridge_subsystem_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UART bridge subsystem: %d", err);
        return err;
    }

    /* 2. Initialize Cloud Uplink Subsystem (Default instance, Lifecycle, Buffers)
     *    Must run before Wi-Fi so that IP acquisition can resume MQTT cleanly. */
    cloud_uplink_default_init();

    /* 3. Initialize Wi-Fi Manager (NVS, STA/AP, Provisioning FSM, SNTP Callback)
     *    Starts station connection and hooks callbacks. */
    err = wifi_mgr_setup(control_queue, on_wifi_ip_assigned, tb_mqtt_pause, on_sntp_sync);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi manager: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "All Gateway Subsystems initialized successfully in dependency order!");
    return ESP_OK;
}
