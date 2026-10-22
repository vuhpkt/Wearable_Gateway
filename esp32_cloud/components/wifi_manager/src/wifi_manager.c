#include "wifi_manager.h"
#include "wifi_config.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "WIFI_MGR";

esp_err_t wifi_mgr_init(wifi_mgr_ip_callback_t on_ip_cb) {
    nvs_storage_init();

    if (on_ip_cb != NULL) {
        wifi_sta_set_got_ip_cb(on_ip_cb);
    }

    return ESP_OK;
}

esp_err_t wifi_mgr_start(void) {
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = nvs_storage_read_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Loaded Wi-Fi credentials from NVS: %s", ssid);
        return wifi_sta_init_with_creds(ssid, pass);
    }
    ESP_LOGI(TAG, "No valid NVS Wi-Fi credentials. Loading default from wifi_config.h: %s", DEFAULT_WIFI_SSID);
    return wifi_sta_init_with_creds(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
}

bool wifi_mgr_is_connected(void) {
    return wifi_sta_is_connected();
}

bool wifi_mgr_is_time_synced(void) {
    return wifi_sta_is_time_synced();
}

#ifndef HOST_UNIT_TEST
static QueueHandle_t s_prov_queue = NULL;
static prov_fsm_t   s_default_prov_fsm;
static prov_fsm_t  *s_prov_fsm_ptr = &s_default_prov_fsm;
static void (*s_pause_mqtt_cb)(void) = NULL;

static void on_boot_button_pressed(void) {
    prov_event_t evt = { .type = PROV_EVT_BOOT_HOLD_2S, .generation = 0 };
    if (s_prov_queue) {
        xQueueSend(s_prov_queue, &evt, 0);
    }
}

static void on_softap_timeout(void) {
    prov_event_t evt = {
        .type = PROV_EVT_TIMEOUT,
        .generation = s_prov_fsm_ptr ? prov_fsm_get_generation(s_prov_fsm_ptr) : 0
    };
    if (s_prov_queue) {
        xQueueSend(s_prov_queue, &evt, 0);
    }
}

static void on_new_credentials_received(const char *new_ssid, const char *new_pass) {
    prov_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = PROV_EVT_CREDENTIALS_SUBMITTED;
    evt.generation = s_prov_fsm_ptr ? prov_fsm_get_generation(s_prov_fsm_ptr) : 0;
    if (new_ssid) strncpy(evt.ssid, new_ssid, sizeof(evt.ssid) - 1);
    if (new_pass) strncpy(evt.pass, new_pass, sizeof(evt.pass) - 1);
    if (s_prov_queue) {
        xQueueSend(s_prov_queue, &evt, 0);
    }
}

static void adapter_pause_mqtt(void) {
    if (s_pause_mqtt_cb) s_pause_mqtt_cb();
}
static void adapter_stop_sta(void) {
    wifi_sta_stop();
}
static void adapter_start_ap(const char *ssid, const char *password) {
    wifi_ap_start(ssid, password);
}
static void adapter_stop_ap(void) {
    wifi_ap_stop();
}
static void adapter_start_portal(void) {
    wifi_portal_start(on_new_credentials_received);
}
static void adapter_stop_portal(void) {
    wifi_portal_stop();
}
static void adapter_save_nvs(const char *ssid, const char *password) {
    nvs_storage_save_wifi(ssid, password);
}
static bool adapter_read_nvs(char *ssid, size_t s_len, char *password, size_t p_len) {
    return (nvs_storage_read_wifi(ssid, s_len, password, p_len) == ESP_OK);
}
static void adapter_connect_sta(const char *ssid, const char *password) {
    wifi_sta_init_with_creds(ssid, password);
}

static const prov_adapter_t s_prov_adapter = {
    .pause_mqtt = adapter_pause_mqtt,
    .resume_mqtt = NULL,
    .stop_sta = adapter_stop_sta,
    .start_ap = adapter_start_ap,
    .stop_ap = adapter_stop_ap,
    .start_portal = adapter_start_portal,
    .stop_portal = adapter_stop_portal,
    .save_nvs_creds = adapter_save_nvs,
    .read_nvs_creds = adapter_read_nvs,
    .connect_sta = adapter_connect_sta,
};

void wifi_mgr_prov_init(QueueHandle_t control_queue, prov_fsm_t *fsm, void (*pause_mqtt_cb)(void)) {
    s_prov_queue = control_queue;
    s_prov_fsm_ptr = fsm ? fsm : &s_default_prov_fsm;
    s_pause_mqtt_cb = pause_mqtt_cb;

    if (s_prov_fsm_ptr) {
        prov_fsm_init(s_prov_fsm_ptr, &s_prov_adapter);
    }

    boot_button_init(on_boot_button_pressed);
    wifi_ap_set_timeout(300, on_softap_timeout);
}

void wifi_mgr_prov_dispatch(const prov_event_t *evt) {
    if (s_prov_fsm_ptr && evt) {
        prov_fsm_dispatch(s_prov_fsm_ptr, evt);
    }
}

esp_err_t wifi_mgr_setup(QueueHandle_t control_queue,
                         wifi_mgr_ip_callback_t on_ip_cb,
                         void (*pause_mqtt_cb)(void),
                         wifi_sta_sntp_cb_t sntp_cb) {
    esp_err_t err = wifi_mgr_init(on_ip_cb);
    if (err != ESP_OK) {
        return err;
    }

    wifi_mgr_prov_init(control_queue, &s_default_prov_fsm, pause_mqtt_cb);

    if (sntp_cb != NULL) {
        wifi_sta_set_sntp_cb(sntp_cb);
    }

    return wifi_mgr_start();
}
#endif
