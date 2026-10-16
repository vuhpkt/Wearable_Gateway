/*
 * Embedded HTTP Captive Web Portal for Wi-Fi Provisioning
 * ESP-IDF v6.0.2 / ESP32-S3
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HOST_TEST
#include "gateway_host_port.h"
#else
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "wifi_ap.h"
#include "wifi_portal.h"

static const char *TAG = "WIFI_PORTAL";
static httpd_handle_t s_server = NULL;
static wifi_portal_new_creds_cb_t s_creds_cb = NULL;
static volatile bool s_transition_in_progress = false;

static const char PORTAL_HTML[] =
"<!DOCTYPE html>"
"<html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESP32 Wi-Fi Setup</title>"
"<style>"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f172a;color:#f8fafc;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:16px;box-sizing:border-box;}"
".card{background:#1e293b;border-radius:12px;padding:24px;width:100%;max-width:360px;box-shadow:0 4px 6px -1px rgba(0,0,0,0.2);}"
"h2{margin-top:0;color:#38bdf8;text-align:center;}"
"label{display:block;margin-top:16px;font-size:14px;color:#94a3b8;}"
"input{width:100%;padding:10px 12px;margin-top:6px;border-radius:6px;border:1px solid #334155;background:#0f172a;color:#fff;box-sizing:border-box;font-size:16px;}"
"button{width:100%;margin-top:24px;padding:12px;background:#0284c7;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;cursor:pointer;}"
"button:hover{background:#0369a1;}"
"</style></head>"
"<body><div class='card'>"
"<h2>Wi-Fi Setup</h2>"
"<form action='/save' method='POST'>"
"<label for='ssid'>Network Name (SSID)</label>"
"<input type='text' id='ssid' name='ssid' required placeholder='Enter Wi-Fi SSID'>"
"<label for='pass'>Password</label>"
"<input type='password' id='pass' name='pass' placeholder='Enter Wi-Fi password'>"
"<button type='submit'>Save & Connect</button>"
"</form></div></body></html>";

static void url_decode(char *dst, size_t dst_size, const char *src) {
    char a, b;
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* [M2 FIX] Strict query parameter extractor matching exact key names */
static bool get_query_param(const char *body, const char *key, char *out_val, size_t out_val_size) {
    if (!body || !key || !out_val || out_val_size == 0) return false;

    size_t key_len = strlen(key);
    const char *p = body;

    while (*p) {
        bool key_start = (p == body) || (*(p - 1) == '&');
        if (key_start && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val_start = p + key_len + 1;
            const char *val_end = strchr(val_start, '&');
            size_t val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

            if (val_len >= out_val_size) {
                val_len = out_val_size - 1;
            }
            memcpy(out_val, val_start, val_len);
            out_val[val_len] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

#define PROV_TRANS_TASK_STACK_SIZE 3072
static StaticTask_t s_prov_task_tcb;
static StackType_t s_prov_task_stack[PROV_TRANS_TASK_STACK_SIZE / sizeof(StackType_t)];
static TaskHandle_t s_prov_task_handle = NULL;

typedef struct {
    char ssid[33];
    char pass[65];
    bool pending;
} prov_mailbox_t;

static prov_mailbox_t s_prov_mailbox = {0};
static portMUX_TYPE s_prov_lock = portMUX_INITIALIZER_UNLOCKED;

static void prov_worker_task(void *pvParameters) {
    (void)pvParameters;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char local_ssid[33] = {0};
        char local_pass[65] = {0};
        bool has_creds = false;

        portENTER_CRITICAL(&s_prov_lock);
        if (s_prov_mailbox.pending) {
            strncpy(local_ssid, s_prov_mailbox.ssid, sizeof(local_ssid) - 1);
            strncpy(local_pass, s_prov_mailbox.pass, sizeof(local_pass) - 1);
            /* Zero out sensitive credentials in mailbox immediately */
            memset(&s_prov_mailbox, 0, sizeof(s_prov_mailbox));
            has_creds = true;
        }
        portEXIT_CRITICAL(&s_prov_lock);

        if (has_creds) {
            /* Delay to allow HTTP response to flush over TCP before shutting down AP */
            vTaskDelay(pdMS_TO_TICKS(1000));

            if (s_creds_cb != NULL) {
                s_creds_cb(local_ssid, local_pass);
            }

            /* Zero out local sensitive credentials */
            memset(local_ssid, 0, sizeof(local_ssid));
            memset(local_pass, 0, sizeof(local_pass));

            portENTER_CRITICAL(&s_prov_lock);
            s_transition_in_progress = false;
            portEXIT_CRITICAL(&s_prov_lock);
        }
    }
}

static esp_err_t get_index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req) {
    /* [H4 FIX] Guard against duplicate clicks spawning competing transition tasks */
    if (s_transition_in_progress) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Provisioning already in progress");
        return ESP_FAIL;
    }

    /* [H3 FIX] Validate content length and handle fragmented TCP receive */
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char buf[512] = {0};
    int cur_offset = 0;
    int timeout_retries = 0;
    while (cur_offset < total_len) {
        int received = httpd_req_recv(req, buf + cur_offset, total_len - cur_offset);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                timeout_retries++;
                if (timeout_retries < 3) {
                    continue;
                }
            }
            s_transition_in_progress = false;
            httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        cur_offset += received;
    }
    buf[total_len] = '\0';

    char raw_ssid[128] = {0};
    char raw_pass[128] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};

    bool has_ssid = get_query_param(buf, "ssid", raw_ssid, sizeof(raw_ssid));
    bool has_pass = get_query_param(buf, "pass", raw_pass, sizeof(raw_pass));

    if (has_ssid && has_pass) {
        url_decode(ssid, sizeof(ssid), raw_ssid);
        url_decode(pass, sizeof(pass), raw_pass);

        if (strlen(ssid) == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Received new credentials via Web Portal: SSID='%s'", ssid);

        /* [M9 FIX] Disarm SoftAP idle timeout immediately upon valid credential submission */
        wifi_ap_cancel_timeout();

        /* [H12 FIX] Fixed static HTML response prevents Reflected XSS (CWE-79) */
        static const char SUCCESS_HTML[] =
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><title>Saved</title>"
            "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#0f172a;color:#fff;}h2{color:#4ade80;}</style></head>"
            "<body><h2>Credentials Saved!</h2><p>Connecting to Wi-Fi network...</p><p>You may reconnect to your home Wi-Fi.</p></body></html>";

        /* Latch credentials into static resident worker mailbox */
        portENTER_CRITICAL(&s_prov_lock);
        if (s_transition_in_progress || s_prov_mailbox.pending) {
            portEXIT_CRITICAL(&s_prov_lock);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Provisioning already in progress");
            return ESP_FAIL;
        }
        s_transition_in_progress = true;
        s_prov_mailbox.pending = true;
        strncpy(s_prov_mailbox.ssid, ssid, sizeof(s_prov_mailbox.ssid) - 1);
        s_prov_mailbox.ssid[sizeof(s_prov_mailbox.ssid) - 1] = '\0';
        strncpy(s_prov_mailbox.pass, pass, sizeof(s_prov_mailbox.pass) - 1);
        s_prov_mailbox.pass[sizeof(s_prov_mailbox.pass) - 1] = '\0';
        portEXIT_CRITICAL(&s_prov_lock);

        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, SUCCESS_HTML, HTTPD_RESP_USE_STRLEN);

        if (s_prov_task_handle != NULL) {
            xTaskNotifyGive(s_prov_task_handle);
        }

        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID or Password");
    return ESP_FAIL;
}

esp_err_t wifi_portal_start(wifi_portal_new_creds_cb_t on_creds_received) {
    if (s_server != NULL) {
        return ESP_OK; /* Already running */
    }

    portENTER_CRITICAL(&s_prov_lock);
    s_transition_in_progress = false;
    memset(&s_prov_mailbox, 0, sizeof(s_prov_mailbox));
    portEXIT_CRITICAL(&s_prov_lock);
    s_creds_cb = on_creds_received;

    /* Ensure resident static worker is running */
    if (s_prov_task_handle == NULL) {
        s_prov_task_handle = xTaskCreateStaticPinnedToCore(
            prov_worker_task,
            "prov_trans_tsk",
            PROV_TRANS_TASK_STACK_SIZE,
            NULL,
            3,
            s_prov_task_stack,
            &s_prov_task_tcb,
            0  /* Core 0 */
        );
        if (s_prov_task_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create static prov_trans_tsk");
            return ESP_FAIL;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 4096;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_index_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &uri_get);

    httpd_uri_t uri_post = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = post_save_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &uri_post);

    ESP_LOGI(TAG, "Embedded Web Portal started on http://192.168.4.1/");
    return ESP_OK;
}

void wifi_portal_stop(void) {
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        portENTER_CRITICAL(&s_prov_lock);
        s_transition_in_progress = false;
        memset(&s_prov_mailbox, 0, sizeof(s_prov_mailbox));
        portEXIT_CRITICAL(&s_prov_lock);
        ESP_LOGI(TAG, "Web Portal stopped.");
    }
}

#ifdef HOST_TEST
/* Host test seam for Task 5b verification */
bool wifi_portal_test_is_busy(void) {
    portENTER_CRITICAL(&s_prov_lock);
    bool busy = s_transition_in_progress || s_prov_mailbox.pending;
    portEXIT_CRITICAL(&s_prov_lock);
    return busy;
}

bool wifi_portal_test_mailbox_has_creds(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len) {
    portENTER_CRITICAL(&s_prov_lock);
    bool pending = s_prov_mailbox.pending;
    if (pending) {
        if (out_ssid && ssid_len > 0) {
            strncpy(out_ssid, s_prov_mailbox.ssid, ssid_len - 1);
            out_ssid[ssid_len - 1] = '\0';
        }
        if (out_pass && pass_len > 0) {
            strncpy(out_pass, s_prov_mailbox.pass, pass_len - 1);
            out_pass[pass_len - 1] = '\0';
        }
    }
    portEXIT_CRITICAL(&s_prov_lock);
    return pending;
}

void wifi_portal_test_step_worker(void) {
    char local_ssid[33] = {0};
    char local_pass[65] = {0};
    bool has_creds = false;

    portENTER_CRITICAL(&s_prov_lock);
    if (s_prov_mailbox.pending) {
        strncpy(local_ssid, s_prov_mailbox.ssid, sizeof(local_ssid) - 1);
        strncpy(local_pass, s_prov_mailbox.pass, sizeof(local_pass) - 1);
        memset(&s_prov_mailbox, 0, sizeof(s_prov_mailbox));
        has_creds = true;
    }
    portEXIT_CRITICAL(&s_prov_lock);

    if (has_creds) {
        if (s_creds_cb != NULL) {
            s_creds_cb(local_ssid, local_pass);
        }
        memset(local_ssid, 0, sizeof(local_ssid));
        memset(local_pass, 0, sizeof(local_pass));

        portENTER_CRITICAL(&s_prov_lock);
        s_transition_in_progress = false;
        portEXIT_CRITICAL(&s_prov_lock);
    }
}

esp_err_t wifi_portal_test_post_save(httpd_req_t *req) {
    return post_save_handler(req);
}

TaskHandle_t wifi_portal_test_get_worker_handle(void) {
    return s_prov_task_handle;
}

void wifi_portal_test_reset(void) {
    portENTER_CRITICAL(&s_prov_lock);
    s_transition_in_progress = false;
    memset(&s_prov_mailbox, 0, sizeof(s_prov_mailbox));
    portEXIT_CRITICAL(&s_prov_lock);
    s_server = NULL;
    s_creds_cb = NULL;
}
#endif
