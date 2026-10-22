/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "thingsboard_mqtt.h"

/* ══════════════════════════════════════════════════════════════
 *  MQTT Lifecycle & PUBACK Manager Implementation
 * ════════════════════════════════════════════════════════════ */

void mqtt_lifecycle_init(mqtt_lifecycle_mgr_t *mgr) {
    if (!mgr) return;
    memset(mgr, 0, sizeof(*mgr));
}

void mqtt_lifecycle_on_broker_connected(mqtt_lifecycle_mgr_t *mgr) {
    if (!mgr) return;
    mgr->mqtt_connected = true;
    mgr->mqtt_generation++;
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        mgr->nodes[i].state = NODE_MQTT_STATE_IDLE;
        mgr->nodes[i].connect_msg_id = 0;
        mgr->nodes[i].attr_msg_id = 0;
    }
}

void mqtt_lifecycle_on_broker_disconnected(mqtt_lifecycle_mgr_t *mgr) {
    if (!mgr) return;
    mgr->mqtt_connected = false;
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        mgr->nodes[i].state = NODE_MQTT_STATE_IDLE;
        mgr->nodes[i].connect_msg_id = 0;
        mgr->nodes[i].attr_msg_id = 0;
    }
}

int mqtt_lifecycle_start_announce(mqtt_lifecycle_mgr_t *mgr,
                                  uint8_t node_id,
                                  const uint8_t *mac,
                                  uint32_t session_id,
                                  int pub_msg_id) {
    if (!mgr || node_id >= MAX_NODES_SUPPORTED || !mac || session_id == 0 || pub_msg_id <= 0) {
        return -1;
    }
    node_mqtt_tracker_t *n = &mgr->nodes[node_id];
    memcpy(n->mac, mac, 6);
    n->session_id = session_id;
    n->connect_msg_id = pub_msg_id;
    n->attr_msg_id = 0;
    n->generation = mgr->mqtt_generation;
    n->state = NODE_MQTT_STATE_WAIT_CONNECT_ACK;
    mgr->connect_published_count++;
    return 0;
}

int mqtt_lifecycle_handle_puback(mqtt_lifecycle_mgr_t *mgr,
                                 int pub_msg_id,
                                 uint8_t *out_node_id,
                                 bool *out_ready_for_attributes) {
    if (!mgr || pub_msg_id <= 0) {
        return -1;
    }
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        node_mqtt_tracker_t *n = &mgr->nodes[i];
        if (n->generation != mgr->mqtt_generation) {
            continue; /* ACK from stale generation */
        }
        if (n->state == NODE_MQTT_STATE_WAIT_CONNECT_ACK && n->connect_msg_id == pub_msg_id) {
            n->state = NODE_MQTT_STATE_WAIT_ATTR_ACK;
            mgr->connect_ack_count++;
            if (out_node_id) *out_node_id = i;
            if (out_ready_for_attributes) *out_ready_for_attributes = true;
            return 0;
        }
        if (n->state == NODE_MQTT_STATE_WAIT_ATTR_ACK && n->attr_msg_id == pub_msg_id) {
            n->state = NODE_MQTT_STATE_ACTIVE;
            mgr->attr_ack_count++;
            if (out_node_id) *out_node_id = i;
            if (out_ready_for_attributes) *out_ready_for_attributes = false;
            return 0;
        }
    }
    mgr->stale_ack_drops++;
    return -2; /* Unmatched or stale msg_id */
}

int mqtt_lifecycle_set_attributes_msg_id(mqtt_lifecycle_mgr_t *mgr,
                                         uint8_t node_id,
                                         int pub_msg_id) {
    if (!mgr || node_id >= MAX_NODES_SUPPORTED || pub_msg_id <= 0) {
        return -1;
    }
    node_mqtt_tracker_t *n = &mgr->nodes[node_id];
    if (n->generation != mgr->mqtt_generation || n->state != NODE_MQTT_STATE_WAIT_ATTR_ACK) {
        return -2;
    }
    n->attr_msg_id = pub_msg_id;
    mgr->attr_published_count++;
    return 0;
}

bool mqtt_lifecycle_is_node_active(const mqtt_lifecycle_mgr_t *mgr,
                                   uint8_t node_id,
                                   uint32_t session_id) {
    if (!mgr || node_id >= MAX_NODES_SUPPORTED || session_id == 0 || !mgr->mqtt_connected) {
        return false;
    }
    const node_mqtt_tracker_t *n = &mgr->nodes[node_id];
    return (n->generation == mgr->mqtt_generation &&
            n->session_id == session_id &&
            n->state == NODE_MQTT_STATE_ACTIVE);
}

void mqtt_lifecycle_handle_node_disconnect(mqtt_lifecycle_mgr_t *mgr,
                                           uint8_t node_id) {
    if (!mgr || node_id >= MAX_NODES_SUPPORTED) return;
    node_mqtt_tracker_t *n = &mgr->nodes[node_id];
    memset(n, 0, sizeof(*n));
    n->state = NODE_MQTT_STATE_IDLE;
}

bool mqtt_lifecycle_can_publish_telemetry(mqtt_lifecycle_mgr_t *mgr,
                                          uint8_t node_id,
                                          uint32_t session_id,
                                          uint64_t event_utc_ms,
                                          uint64_t current_utc_ms,
                                          uint32_t ttl_ms) {
    if (!mgr || !mqtt_lifecycle_is_node_active(mgr, node_id, session_id)) {
        if (mgr) mgr->telemetry_blocked_count++;
        return false;
    }
    if (event_utc_ms == 0 || current_utc_ms < event_utc_ms) {
        mgr->telemetry_blocked_count++;
        return false;
    }
    uint64_t age = current_utc_ms - event_utc_ms;
    if (age >= (uint64_t)ttl_ms) {
        mgr->ttl_dropped_count++;
        return false;
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════
 *  ESP-IDF Hardware MQTT Client (Target Only)
 * ════════════════════════════════════════════════════════════ */
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)

/* FreeRTOS & ESP-IDF System Headers */
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>

static const char *TAG = "tb_mqtt";

#define MQTT_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_event_group = NULL;
static StaticEventGroup_t s_event_group_buf;
static bool s_is_connected = false;
static bool s_is_paused = false;
static tb_mqtt_connect_cb_t s_connect_cb = NULL;
static tb_mqtt_publish_cb_t s_publish_cb = NULL;
static tb_mqtt_data_cb_t s_data_cb = NULL;

void tb_mqtt_register_connect_callback(tb_mqtt_connect_cb_t cb) {
    s_connect_cb = cb;
}

void tb_mqtt_register_publish_callback(tb_mqtt_publish_cb_t cb) {
    s_publish_cb = cb;
}

void tb_mqtt_register_data_callback(tb_mqtt_data_cb_t cb) {
    s_data_cb = cb;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to Broker successfully! [State: READY]");
        s_is_connected = true;
        if (s_client != NULL) {
            esp_mqtt_client_subscribe(s_client, "v1/gateway/attributes", 1);
            ESP_LOGI(TAG, "Subscribed to v1/gateway/attributes (QoS 1)");
        }
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        if (s_connect_cb != NULL) {
            s_connect_cb();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected from Broker! Waiting auto-reconnect...");
        s_is_connected = false;
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT Publish ACK received (msg_id=%d)", event->msg_id);
        if (s_publish_cb != NULL) {
            s_publish_cb(event->msg_id);
        }
        break;
    case MQTT_EVENT_DATA:
        if (s_data_cb != NULL && event->topic != NULL && event->data != NULL) {
            s_data_cb(event->topic, event->topic_len, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error event encountered (type=%d)", event->error_handle->error_type);
        break;
    default:
        break;
    }
}

esp_err_t tb_mqtt_init(const char *broker_uri, const char *access_token) {
    if (broker_uri == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreateStatic(&s_event_group_buf);
        if (s_event_group == NULL)
            return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t config = {
        .broker.address.uri = broker_uri,
        .credentials.username = access_token,
        .network.timeout_ms = 5000,
        .session.keepalive = 15};

    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize esp_mqtt_client handle");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT Client initialized -> Target Broker: %s", broker_uri);
    return ESP_OK;
}

bool tb_mqtt_is_connected(void) { return s_is_connected; }

esp_err_t tb_mqtt_wait_connected(TickType_t xTicksToWait) {
    if (s_event_group == NULL)
        return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(s_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, xTicksToWait);
    return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int tb_mqtt_publish(const char *topic, const char *json_payload, int qos) {
    if (s_client == NULL || json_payload == NULL || !s_is_connected)
        return -1;
    int len = (int)strlen(json_payload);
    int msg_id = esp_mqtt_client_publish(s_client, topic, json_payload, len, qos, 0);
    return msg_id;
}

void tb_mqtt_stop(void) {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_is_connected = false;
    s_is_paused = false;
}

void tb_mqtt_pause(void) {
    if (s_client != NULL && !s_is_paused) {
        ESP_LOGI(TAG, "⏸ Pausing MQTT client (Wi-Fi offline / Provisioning mode)...");
        esp_mqtt_client_stop(s_client);
        s_is_paused = true;
        s_is_connected = false;
        if (s_event_group != NULL) {
            xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        }
    }
}

void tb_mqtt_resume(void) {
    if (s_client != NULL && s_is_paused) {
        ESP_LOGI(TAG, "▶ Resuming MQTT client connection...");
        esp_err_t err = esp_mqtt_client_start(s_client);
        if (err == ESP_OK) {
            s_is_paused = false;
        } else {
            ESP_LOGE(TAG, "Failed to resume MQTT client: %s", esp_err_to_name(err));
        }
    }
}

#endif
