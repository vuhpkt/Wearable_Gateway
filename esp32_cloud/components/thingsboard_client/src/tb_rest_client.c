/*
 * Copyright (c) 2026 Wearable Gateway Project
 * ThingsBoard REST Client Module for Background SERVER_SCOPE Synchronization
 * Production-grade: Dynamic UUID resolution, 10-node cache, zero dynamic memory
 */

#include "tb_rest_client.h"
#include "thingsboard_config.h"
#include "uart_frame_def.h"
#include "wifi_sta_mgr.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>

static const char *TAG = "tb_rest";

typedef struct {
    bool desired_connected;
    bool dirty;
    uint32_t generation;
} node_desired_state_t;

static node_desired_state_t s_desired_states[MAX_NODES_SUPPORTED] = {{0}};
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_rest_task_handle = NULL;
static char s_jwt_token[1024] = {0};
static time_t s_token_expiry = 0;

/* Cache UUID độc lập cho 10 Node (index 0..9 ứng với Node1..Node10) */
static char s_node_uuid_cache[MAX_NODES_SUPPORTED][37] = {{0}};

/* Static buffers để giảm tải stack của tb_rest_task */
static char s_resp_buf[2048] = {0};
static char s_auth_header[1100] = {0};

void *tb_rest_get_task_handle(void) {
    return (void *)s_rest_task_handle;
}

void tb_rest_set_node_connected(uint8_t node_id, bool connected) {
    if (node_id >= MAX_NODES_SUPPORTED) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_desired_states[node_id].desired_connected = connected;
    s_desired_states[node_id].dirty = true;
    s_desired_states[node_id].generation++;
    portEXIT_CRITICAL(&s_state_lock);

    if (s_rest_task_handle != NULL) {
        xTaskNotifyGive(s_rest_task_handle);
    }
}

bool tb_rest_parse_device_uuid(const char *json_str, char *out_uuid, size_t out_uuid_len) {
    if (json_str == NULL || out_uuid == NULL || out_uuid_len < 37) {
        return false;
    }

    /* 1. Tìm đối tượng "id":{ */
    const char *id_obj = strstr(json_str, "\"id\":{");
    if (id_obj == NULL) {
        id_obj = strstr(json_str, "\"id\": {");
        if (id_obj == NULL) {
            return false;
        }
    }

    /* 2. Tìm ngoặc đóng của đối tượng "id" */
    const char *obj_end = strchr(id_obj, '}');
    if (obj_end == NULL) {
        return false;
    }

    /* 3. Trong đối tượng "id", tìm trường "id":"<uuid>" */
    const char *id_key = "\"id\":\"";
    const char *pos = strstr(id_obj, id_key);
    size_t key_len = strlen(id_key);
    if (pos == NULL || pos > obj_end) {
        id_key = "\"id\": \"";
        pos = strstr(id_obj, id_key);
        key_len = strlen(id_key);
        if (pos == NULL || pos > obj_end) {
            return false;
        }
    }

    pos += key_len;
    const char *quote_end = strchr(pos, '"');
    if (quote_end == NULL || quote_end > obj_end) {
        return false;
    }

    size_t uuid_len = (size_t)(quote_end - pos);
    if (uuid_len != 36) {
        return false; /* UUID chuẩn 36 ký tự */
    }

    memcpy(out_uuid, pos, 36);
    out_uuid[36] = '\0';
    return true;
}

typedef struct {
    char *buf;
    size_t max_len;
    size_t offset;
} http_resp_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_resp_buffer_t *resp = (http_resp_buffer_t *)evt->user_data;
        if (resp && resp->buf && resp->offset < resp->max_len - 1) {
            size_t copy_len = evt->data_len;
            if (resp->offset + copy_len >= resp->max_len) {
                copy_len = resp->max_len - 1 - resp->offset;
            }
            memcpy(resp->buf + resp->offset, evt->data, copy_len);
            resp->offset += copy_len;
            resp->buf[resp->offset] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t tb_rest_login(void) {
    char post_data[128];
    snprintf(post_data, sizeof(post_data), "{\"username\":\"%s\",\"password\":\"%s\"}",
             TB_REST_USERNAME, TB_REST_PASSWORD);

    char url[128];
    snprintf(url, sizeof(url), "%s/api/auth/login", TB_REST_BASE_URL);

    memset(s_resp_buf, 0, sizeof(s_resp_buf));
    http_resp_buffer_t resp_ctx = {
        .buf = s_resp_buf,
        .max_len = sizeof(s_resp_buf),
        .offset = 0
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for REST login");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            const char *token_key = "\"token\":\"";
            char *pos = strstr(s_resp_buf, token_key);
            if (pos) {
                pos += strlen(token_key);
                char *end = strchr(pos, '"');
                if (end && (size_t)(end - pos) < sizeof(s_jwt_token)) {
                    size_t tok_len = end - pos;
                    memcpy(s_jwt_token, pos, tok_len);
                    s_jwt_token[tok_len] = '\0';
                    time(&s_token_expiry);
                    s_token_expiry += 7200; /* 2 hours cache */
                    ESP_LOGI(TAG, "🔑 ThingsBoard REST login successful (JWT Cached 2h)");
                    esp_http_client_cleanup(client);
                    return ESP_OK;
                }
            }
            ESP_LOGW(TAG, "TB REST Login returned 200 but token not found in: %s", s_resp_buf);
        } else {
            ESP_LOGW(TAG, "TB REST Login HTTP Status %d: %s", status, s_resp_buf);
        }
    } else {
        ESP_LOGW(TAG, "TB REST Login connection error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

static esp_err_t tb_rest_resolve_device_uuid(uint8_t node_id, char *out_uuid, size_t out_uuid_len) {
    if (node_id >= MAX_NODES_SUPPORTED || out_uuid == NULL || out_uuid_len < 37) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_jwt_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[160];
    snprintf(url, sizeof(url), "%s/api/tenant/devices?deviceName=Node%u",
             TB_REST_BASE_URL, node_id + 1);

    memset(s_resp_buf, 0, sizeof(s_resp_buf));
    http_resp_buffer_t resp_ctx = {
        .buf = s_resp_buf,
        .max_len = sizeof(s_resp_buf),
        .offset = 0
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    int auth_len = snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", s_jwt_token);
    if (auth_len < 0 || (size_t)auth_len >= sizeof(s_auth_header)) {
        ESP_LOGE(TAG, "auth_header buffer overflow");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    esp_http_client_set_header(client, "X-Authorization", s_auth_header);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            if (tb_rest_parse_device_uuid(s_resp_buf, out_uuid, out_uuid_len)) {
                ESP_LOGI(TAG, "🔑 [REST] Successfully resolved UUID for Node%u: %s",
                         node_id + 1, out_uuid);
                esp_http_client_cleanup(client);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Unable to parse UUID from response for Node%u: %s", node_id + 1, s_resp_buf);
            }
        } else if (status == 404) {
            ESP_LOGW(TAG, "Node%u does not exist on ThingsBoard (404)", node_id + 1);
        } else if (status == 401) {
            ESP_LOGW(TAG, "Token expired while resolving UUID for Node%u (401)", node_id + 1);
            s_jwt_token[0] = '\0';
        } else {
            ESP_LOGW(TAG, "Resolve UUID Node%u HTTP status %d: %s", node_id + 1, status, s_resp_buf);
        }
    } else {
        ESP_LOGW(TAG, "Resolve UUID Node%u connection error: %s", node_id + 1, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

static esp_err_t tb_rest_set_attribute(const char *device_uuid, bool connected, int *out_status) {
    if (s_jwt_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/api/plugins/telemetry/DEVICE/%s/attributes/SERVER_SCOPE",
             TB_REST_BASE_URL, device_uuid);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    int auth_len = snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", s_jwt_token);
    if (auth_len < 0 || (size_t)auth_len >= sizeof(s_auth_header)) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    esp_http_client_set_header(client, "X-Authorization", s_auth_header);

    const char *body = connected ? "{\"connected\":true}" : "{\"connected\":false}";
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (out_status != NULL) {
            *out_status = status;
        }
        if (status == 200) {
            ESP_LOGI(TAG, "✅ [REST] SERVER_SCOPE connected=%s (Device: %s)",
                     connected ? "true" : "false", device_uuid);
            esp_http_client_cleanup(client);
            return ESP_OK;
        } else if (status == 401) {
            ESP_LOGW(TAG, "TB REST Token 401 Unauthorized - Clearing cache to refresh");
            s_jwt_token[0] = '\0';
        } else {
            ESP_LOGW(TAG, "TB REST set attributes HTTP Status %d", status);
        }
    } else {
        ESP_LOGW(TAG, "TB REST set attributes connection error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

static void tb_rest_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        /* Wait for notification from tb_rest_set_node_connected() or timeout every 2 seconds */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

        if (!wifi_sta_is_connected()) {
            continue;
        }

        for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
            bool need_sync = false;
            bool target_state = false;
            uint32_t gen = 0;

            portENTER_CRITICAL(&s_state_lock);
            if (s_desired_states[i].dirty) {
                need_sync = true;
                target_state = s_desired_states[i].desired_connected;
                gen = s_desired_states[i].generation;
            }
            portEXIT_CRITICAL(&s_state_lock);

            if (!need_sync) {
                continue;
            }

            /* Ensure JWT token is valid */
            time_t now = 0;
            time(&now);
            if (s_jwt_token[0] == '\0' || (now > 1700000000 && now >= s_token_expiry)) {
                if (tb_rest_login() != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    break; /* Try next round */
                }
            }

            /* Dynamically resolve device UUID from ThingsBoard if not cached */
            if (s_node_uuid_cache[i][0] == '\0') {
                char resolved[37] = {0};
                if (tb_rest_resolve_device_uuid(i, resolved, sizeof(resolved)) == ESP_OK) {
                    memcpy(s_node_uuid_cache[i], resolved, 37);
                } else {
                    ESP_LOGW(TAG, "Could not resolve UUID for Node%u. Will retry...", i + 1);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
            }

            int http_status = 0;
            esp_err_t set_err = tb_rest_set_attribute(s_node_uuid_cache[i], target_state, &http_status);
            if (set_err == ESP_OK) {
                portENTER_CRITICAL(&s_state_lock);
                /* Only clear dirty flag if no newer state was posted while HTTP was in flight */
                if (s_desired_states[i].generation == gen) {
                    s_desired_states[i].dirty = false;
                }
                portEXIT_CRITICAL(&s_state_lock);
            } else {
                if (http_status == 404) {
                    /* Node was deleted/recreated on ThingsBoard -> invalidate cache to re-resolve */
                    ESP_LOGW(TAG, "UUID for Node%u returned 404. Invalidating cache to refresh.", i + 1);
                    s_node_uuid_cache[i][0] = '\0';
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

esp_err_t tb_rest_client_init(void) {
    BaseType_t ret = xTaskCreatePinnedToCore(
        tb_rest_task,
        "tb_rest_task",
        12288,
        NULL,
        3,
        &s_rest_task_handle,
        0  /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tb_rest_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TB REST Client initialized successfully (Desired-State Engine Ready)");
    return ESP_OK;
}
