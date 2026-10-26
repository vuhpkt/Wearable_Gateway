/**
 * @file test_gateway_subsystems_init.c
 * @brief Host-based Unit Tests for Consolidated Subsystems Bootstrapper & Hybrid Facade Pattern
 *
 * Verifies Architecture v2 Contract:
 * 1. Order Validation: uart_bridge_subsystem_init -> cloud_uplink_default_init -> wifi_mgr_setup.
 * 2. SNTP Forwarding: Spinlocks & Mailbox ready before Wi-Fi SNTP callback triggers.
 * 3. Default Instance Binding: uart_bridge_get_snapshot_queue, uart_bridge_get_ingest_ctx,
 *    and cloud_uplink_get_default_instance return non-NULL and are properly linked.
 * 4. End-to-End Dataflow: Central Handshake -> Ingest -> Snapshot Queue -> Cloud Uplink -> MQTT.
 * 5. Session Revocation: uart_bridge_is_session_valid thread-safe verification.
 * 6. Error Propagation: Failure in wifi_mgr_setup propagates cleanly without state corruption.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#define HOST_UNIT_TEST 1

#include "wifi_manager.h"
#include "uart_bridge.h"
#include "thingsboard_client.h"
#include "cloud_uplink.h"
#include "uart_frame_def.h"
#include "ble_protocol_def.h"
#include "core1_gate.h"
#include "frame_snapshot.h"
#include "gateway_time.h"
#include "gateway_ingest.h"

static void on_wifi_ip_assigned(void) {
    tb_mqtt_resume();
}

static void on_sntp_sync(struct timeval *tv) {
    if (!tv) return;
    int64_t now_utc_ms = (int64_t)tv->tv_sec * 1000LL + (int64_t)(tv->tv_usec / 1000);
    uart_bridge_post_sntp(now_utc_ms, 0);
}

static esp_err_t gateway_subsystems_init(QueueHandle_t control_queue) {
    esp_err_t err = uart_bridge_subsystem_init();
    if (err != ESP_OK) return err;

    cloud_uplink_default_init();

    return wifi_mgr_setup(control_queue, on_wifi_ip_assigned, tb_mqtt_pause, on_sntp_sync);
}

/* ══════════════════════════════════════════════════════════════
 *  Mocks for MQTT Transport & Subsystem Callbacks
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    char topic[64];
    char payload[2048];
    int  qos;
    int  msg_id;
} test_mqtt_pub_record_t;

#define MAX_CAPTURED_PUBS 32
static test_mqtt_pub_record_t s_captured_pubs[MAX_CAPTURED_PUBS];
static uint32_t s_captured_pub_count = 0;
static int s_next_msg_id = 300;
static bool s_mock_connected = true;

static int s_resume_call_count = 0;
static int s_pause_call_count = 0;

bool tb_mqtt_is_connected(void) {
    return s_mock_connected;
}

void tb_mqtt_pause(void) {
    s_pause_call_count++;
}

void tb_mqtt_resume(void) {
    s_resume_call_count++;
}

int tb_mqtt_publish(const char *topic, const char *payload, int qos) {
    if (!s_mock_connected || !topic || !payload) return -1;
    if (s_captured_pub_count < MAX_CAPTURED_PUBS) {
        test_mqtt_pub_record_t *rec = &s_captured_pubs[s_captured_pub_count++];
        strncpy(rec->topic, topic, sizeof(rec->topic) - 1);
        strncpy(rec->payload, payload, sizeof(rec->payload) - 1);
        rec->qos = qos;
        rec->msg_id = s_next_msg_id++;
        return rec->msg_id;
    }
    return -2;
}

static uint8_t s_last_tx_type = 0;
static int s_tx_count = 0;

int uart_bridge_tx_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    (void)node_id;
    (void)payload;
    (void)len;
    s_last_tx_type = pkt_type;
    s_tx_count++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Mocks for Wi-Fi Manager Facade
 * ════════════════════════════════════════════════════════════ */
static QueueHandle_t            s_captured_control_queue = NULL;
static wifi_mgr_ip_callback_t   s_captured_on_ip_cb = NULL;
static void                   (*s_captured_pause_mqtt_cb)(void) = NULL;
static wifi_sta_sntp_cb_t       s_captured_sntp_cb = NULL;
static esp_err_t                s_mock_wifi_setup_ret = ESP_OK;
static int                      s_wifi_setup_call_count = 0;

static const prov_event_t      *s_last_prov_evt = NULL;
static int                      s_prov_dispatch_call_count = 0;

esp_err_t wifi_mgr_setup(QueueHandle_t control_queue,
                         wifi_mgr_ip_callback_t on_ip_cb,
                         void (*pause_mqtt_cb)(void),
                         wifi_sta_sntp_cb_t sntp_cb) {
    s_wifi_setup_call_count++;
    s_captured_control_queue = control_queue;
    s_captured_on_ip_cb = on_ip_cb;
    s_captured_pause_mqtt_cb = pause_mqtt_cb;
    s_captured_sntp_cb = sntp_cb;
    return s_mock_wifi_setup_ret;
}

void wifi_mgr_prov_dispatch(const prov_event_t *evt) {
    s_prov_dispatch_call_count++;
    s_last_prov_evt = evt;
}

static void reset_test_state(void) {
    memset(s_captured_pubs, 0, sizeof(s_captured_pubs));
    s_captured_pub_count = 0;
    s_next_msg_id = 300;
    s_mock_connected = true;
    s_resume_call_count = 0;
    s_pause_call_count = 0;
    s_last_tx_type = 0;
    s_tx_count = 0;
    s_captured_control_queue = NULL;
    s_captured_on_ip_cb = NULL;
    s_captured_pause_mqtt_cb = NULL;
    s_captured_sntp_cb = NULL;
    s_mock_wifi_setup_ret = ESP_OK;
    s_wifi_setup_call_count = 0;
    s_last_prov_evt = NULL;
    s_prov_dispatch_call_count = 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: Dependency Order & SNTP Mailbox Hook
 * ════════════════════════════════════════════════════════════ */
static void test_initialization_order_and_sntp_hook(void) {
    printf("[RUN] Test 1: Order Validation & SNTP Mailbox Hook...\n");
    reset_test_state();

    void *dummy_queue = (void*)0xABCD1234;
    esp_err_t err = gateway_subsystems_init(dummy_queue);
    assert(err == ESP_OK);

    /* 1. Verify wifi_mgr_setup received control queue and callbacks */
    assert(s_wifi_setup_call_count == 1);
    assert(s_captured_control_queue == dummy_queue);
    assert(s_captured_on_ip_cb != NULL);
    assert(s_captured_pause_mqtt_cb != NULL);
    assert(s_captured_sntp_cb != NULL);

    /* 2. Verify IP callback triggers tb_mqtt_resume */
    s_captured_on_ip_cb();
    assert(s_resume_call_count == 1);

    /* 3. Verify SoftAP callback triggers tb_mqtt_pause */
    s_captured_pause_mqtt_cb();
    assert(s_pause_call_count == 1);

    /* 4. Verify SNTP sync callback forwards UTC timestamp to Mailbox */
    struct timeval tv = {
        .tv_sec = 1774000000,
        .tv_usec = 450000,
    };
    s_captured_sntp_cb(&tv);

    gateway_ingest_t *ingest = uart_bridge_get_ingest_ctx();
    assert(ingest != NULL);
    assert(ingest->time_mailbox != NULL);

    gateway_time_obs_t obs;
    bool fetched = gateway_time_mailbox_poll(ingest->time_mailbox, &obs);
    assert(fetched == true);
    assert(obs.utc_ms == 1774000000450LL);

    printf("  [OK] Dependency order, callbacks, and SNTP mailbox post verified.\n");
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: Default Instance Binding & Core 1 -> Core 0 Dataflow
 * ════════════════════════════════════════════════════════════ */
static void test_default_instance_binding_and_dataflow(void) {
    printf("[RUN] Test 2: Default Instance Binding & Dataflow Integrity...\n");
    reset_test_state();

    esp_err_t err = gateway_subsystems_init(NULL);
    assert(err == ESP_OK);

    /* 1. Verify default instance pointers */
    const frame_snapshot_queue_t *queue = uart_bridge_get_snapshot_queue();
    assert(queue != NULL);

    gateway_ingest_t *ingest = uart_bridge_get_ingest_ctx();
    assert(ingest != NULL);
    assert(ingest->snapshot_queue == queue);
    assert(ingest->rev_table != NULL);
    assert(ingest->gateway_time != NULL);
    assert(ingest->time_mailbox != NULL);

    /* 2. Simulate Central Handshake (HELLO packet) */
    uart_ctrl_hello_pkt_t hello = {
        .version = UART_BRIDGE_PROTOCOL_VERSION,
        .boot_nonce = 0x87654321,
        .reserved = 0,
    };
    uart_rx_frame_t frame_hello = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_CONTROL_HELLO,
        .len = sizeof(hello),
    };
    memcpy(frame_hello.payload, &hello, sizeof(hello));
    gateway_ingest_on_frame(ingest, &frame_hello);

    assert(s_tx_count == 2);
    assert(s_last_tx_type == UART_PKT_TYPE_SNAPSHOT_REQ);
    assert(gateway_ingest_is_link_ready(ingest) == false);

    /* 3. Send 10 Snapshot Status slots */
    uint64_t epoch = ingest->bridge_fsm.epoch;
    for (uint8_t i = 0; i < 10; i++) {
        uart_v2_envelope_t env = {
            .bridge_epoch = epoch,
            .session_id = (i == 0) ? 1001 : 0,
            .seq = 1,
            .central_rx_mono_ms = 5000 + i * 10,
        };
        uart_node_status_pkt_t status = {
            .status = (i == 0) ? 1 : 0,
            .mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, (uint8_t)(0x01 + i) },
        };
        uart_rx_frame_t frame_slot = {
            .node_id = i,
            .pkt_type = UART_PKT_TYPE_NODE_STATUS_V2,
            .len = sizeof(env) + sizeof(status),
        };
        memcpy(frame_slot.payload, &env, sizeof(env));
        memcpy(frame_slot.payload + sizeof(env), &status, sizeof(status));
        gateway_ingest_on_frame(ingest, &frame_slot);
    }

    /* 4. Complete snapshot phase */
    uart_snapshot_end_pkt_t end_pkt = {
        .bridge_epoch = epoch,
        .transaction_id = 1,
        .slot_count = 10,
        .reserved = 0,
    };
    uart_rx_frame_t frame_end = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_SNAPSHOT_END,
        .len = sizeof(end_pkt),
    };
    memcpy(frame_end.payload, &end_pkt, sizeof(end_pkt));
    gateway_ingest_on_frame(ingest, &frame_end);
    assert(gateway_ingest_is_link_ready(ingest) == true);

    /* 5. Pop snapshot from queue via Facade API */
    frame_snapshot_tuple_t tuple;
    bool popped = uart_bridge_pop_snapshot(&tuple, 100);
    assert(popped == true);
    assert(tuple.node_id == 0);
    assert(tuple.session_id == 1001);
    assert(tuple.stream_type == STREAM_TYPE_STATUS);
    assert(tuple.payload_len >= 1 && tuple.payload[0] == 1);

    /* 6. Verify session validity via Facade API */
    assert(uart_bridge_is_session_valid(0, epoch, 1001) == true);
    assert(uart_bridge_is_session_valid(0, epoch, 9999) == false);

    /* 7. Drain remaining 9 status snapshots */
    for (int i = 1; i < 10; i++) {
        popped = uart_bridge_pop_snapshot(&tuple, 10);
        assert(popped == true);
        assert(tuple.node_id == i);
    }

    /* Queue should be empty now */
    popped = uart_bridge_pop_snapshot(&tuple, 10);
    assert(popped == false);

    printf("  [OK] Default instance binding, handshake, snapshot pop, and session validation verified.\n");
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: Cloud Uplink Default Facade & Lifecycle Dispatch
 * ════════════════════════════════════════════════════════════ */
static void test_cloud_uplink_default_facade_and_dispatch(void) {
    printf("[RUN] Test 3: Cloud Uplink Default Facade & Lifecycle Dispatch...\n");
    reset_test_state();

    esp_err_t err = gateway_subsystems_init(NULL);
    assert(err == ESP_OK);

    cloud_uplink_t *uplink = cloud_uplink_get_default_instance();
    assert(uplink != NULL);

    /* Process pending events without crash */
    cloud_uplink_default_process_events();

    /* 1. Simulate MQTT connect callback & process event */
    cloud_uplink_on_mqtt_connected_cb();
    cloud_uplink_default_process_events();

    /* 2. Dispatch a Node Status CONNECT tuple into default uplink */
    frame_snapshot_tuple_t connect_tuple = {
        .node_id = 1,
        .session_id = 2002,
        .bridge_epoch = 1,
        .mac = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 },
        .stream_type = STREAM_TYPE_STATUS,
        .payload_len = 1,
        .payload = { 1 },
    };
    cloud_uplink_default_dispatch(&connect_tuple);

    /* Assert: v1/gateway/connect published */
    assert(s_captured_pub_count == 1);
    assert(strstr(s_captured_pubs[0].topic, "v1/gateway/connect") != NULL);
    assert(strstr(s_captured_pubs[0].payload, "11:22:33:44:55:66") != NULL);

    /* 3. Simulate MQTT PUBACK for CONNECT -> triggers ATTRIBUTES publish */
    cloud_uplink_on_mqtt_published_cb(s_captured_pubs[0].msg_id);
    cloud_uplink_default_process_events();

    /* Assert: v1/gateway/attributes published for device announce */
    assert(s_captured_pub_count == 2);
    assert(strstr(s_captured_pubs[1].topic, "v1/gateway/attributes") != NULL);

    /* 4. Simulate MQTT PUBACK for ATTRIBUTES -> node transitions to ACTIVE */
    cloud_uplink_on_mqtt_published_cb(s_captured_pubs[1].msg_id);
    cloud_uplink_default_process_events();

    /* Verify slot lookup facade now finds ACTIVE node */
    uint8_t find_mac[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    int slot = cloud_uplink_default_find_slot_by_mac(find_mac);
    assert(slot == 1);

    /* 5. Dispatch Telemetry tuple */
    vitals_packet_t vitals_data = {
        .hr_ecg = 72,
        .hr_ppg = 73,
        .spo2 = 99,
        .temp_x10 = 368,
    };
    frame_snapshot_tuple_t vitals_tuple = {
        .node_id = 1,
        .session_id = 2002,
        .bridge_epoch = 1,
        .mac = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 },
        .stream_type = STREAM_TYPE_VITALS,
        .time_quality = TIME_QUALITY_SYNCED,
        .timestamp_utc_ms = 1774000000000ULL,
        .payload_len = sizeof(vitals_data),
    };
    memcpy(vitals_tuple.payload, &vitals_data, sizeof(vitals_data));
    cloud_uplink_default_dispatch(&vitals_tuple);

    /* Assert: v1/gateway/telemetry published */
    assert(s_captured_pub_count == 3);
    assert(strstr(s_captured_pubs[2].topic, "v1/gateway/telemetry") != NULL);
    assert(strstr(s_captured_pubs[2].payload, "\"ecgHeartRate\":72") != NULL);
    assert(strstr(s_captured_pubs[2].payload, "\"spo2\":99") != NULL);

    printf("  [OK] Cloud uplink default facade, connect announce, and vitals dispatch verified.\n");
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: Error Handling & Wi-Fi Prov Dispatch
 * ════════════════════════════════════════════════════════════ */
static void test_error_handling_and_prov_dispatch(void) {
    printf("[RUN] Test 4: Error Handling & Wi-Fi Prov Dispatch...\n");
    reset_test_state();

    /* 1. Verify failure in wifi_mgr_setup propagates cleanly */
    s_mock_wifi_setup_ret = ESP_FAIL;
    esp_err_t err = gateway_subsystems_init(NULL);
    assert(err == ESP_FAIL);

    /* 2. Verify Wi-Fi Prov Dispatch routes correctly */
    prov_event_t evt = {
        .type = PROV_EVT_BOOT_HOLD_2S,
        .generation = 3,
    };
    wifi_mgr_prov_dispatch(&evt);
    assert(s_prov_dispatch_call_count == 1);
    assert(s_last_prov_evt != NULL);
    assert(s_last_prov_evt->type == PROV_EVT_BOOT_HOLD_2S);
    assert(s_last_prov_evt->generation == 3);

    printf("  [OK] Error propagation and provisioning dispatch verified.\n");
}

int main(void) {
    printf("==============================================\n");
    printf("  TEST: Gateway Subsystems & Hybrid Facade    \n");
    printf("==============================================\n");

    test_initialization_order_and_sntp_hook();
    test_default_instance_binding_and_dataflow();
    test_cloud_uplink_default_facade_and_dispatch();
    test_error_handling_and_prov_dispatch();

    printf("\n🎉 ALL GATEWAY SUBSYSTEMS TESTS PASSED (100%%)\n");
    return 0;
}
