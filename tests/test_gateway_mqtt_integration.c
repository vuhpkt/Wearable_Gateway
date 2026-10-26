/**
 * @file test_gateway_mqtt_integration.c
 * @brief Host-based Unit Tests for Cloud Uplink & MQTT Announce Barrier (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Announce Barrier: CONNECT (QoS 1) -> PUBACK -> ATTRIBUTES (QoS 1) -> PUBACK -> ACTIVE.
 * 2. Attributes are NEVER published before CONNECT PUBACK.
 * 3. Telemetry is strictly blocked until node is ACTIVE.
 * 4. Telemetry with TTL >= 5000ms is dropped.
 * 5. Disconnect immediately revokes ACTIVE state and blocks telemetry.
 * 6. Duplicate or stale PUBACK does not re-trigger attributes publish.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "cloud_uplink.h"

/* Mock MQTT Transport State */
static bool s_mock_connected = true;
static int s_next_msg_id = 100;

typedef struct {
    char topic[128];
    char payload[1024];
    int qos;
    int msg_id;
} mock_pub_record_t;

#define MAX_RECORDS 32
static mock_pub_record_t s_records[MAX_RECORDS];
static int s_record_count = 0;

int tb_mqtt_publish(const char *topic, const char *json_payload, int qos) {
    if (!s_mock_connected) return -1;
    if (s_record_count < MAX_RECORDS) {
        strncpy(s_records[s_record_count].topic, topic, sizeof(s_records[s_record_count].topic) - 1);
        strncpy(s_records[s_record_count].payload, json_payload, sizeof(s_records[s_record_count].payload) - 1);
        s_records[s_record_count].qos = qos;
        s_records[s_record_count].msg_id = s_next_msg_id++;
        return s_records[s_record_count++].msg_id;
    }
    return -1;
}

bool tb_mqtt_is_connected(void) {
    return s_mock_connected;
}

static void reset_mock_records(void) {
    s_record_count = 0;
    memset(s_records, 0, sizeof(s_records));
}

static void test_announce_barrier_and_telemetry_gate(void) {
    printf("[RUN] Test 1: Announce Barrier (CONNECT -> ACK -> ATTR -> ACK -> ACTIVE)...\n");
    cloud_uplink_t uplink;
    cloud_uplink_init(&uplink);

    /* Broker connects */
    cloud_mqtt_evt_t evt_conn = { .type = CLOUD_MQTT_EVT_CONNECTED, .msg_id = 0 };
    cloud_uplink_post_mqtt_event(&uplink, &evt_conn);
    cloud_uplink_process_events(&uplink);

    reset_mock_records();

    uint8_t mac[6] = { 0xFE, 0xC3, 0x3C, 0x89, 0xE7, 0x8E };
    frame_snapshot_tuple_t tuple_conn = {
        .node_id = 0,
        .stream_type = STREAM_TYPE_STATUS,
        .session_id = 42,
        .bridge_epoch = 100ULL,
        .timestamp_utc_ms = 10000ULL,
    };
    memcpy(tuple_conn.mac, mac, 6);

    /* Step 1: Node status CONNECT arrives */
    cloud_uplink_handle_node_status(&uplink, &tuple_conn, true);

    /* Assert: CONNECT topic published, but ATTRIBUTES is NOT published yet! */
    assert(s_record_count == 1);
    assert(strcmp(s_records[0].topic, "v1/gateway/connect") == 0);
    int connect_msg_id = s_records[0].msg_id;

    /* Assert: Node is NOT active yet */
    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 0, 42) == false);

    /* Step 2: Telemetry arrives while waiting for CONNECT ACK -> MUST be dropped */
    vitals_packet_t v = { .hr_ecg = 75, .hr_ppg = 74, .spo2 = 98, .temp_x10 = 365 };
    frame_snapshot_tuple_t tuple_vitals = {
        .node_id = 0,
        .stream_type = STREAM_TYPE_VITALS,
        .session_id = 42,
        .bridge_epoch = 100ULL,
        .timestamp_utc_ms = 10500ULL,
        .time_quality = TIME_QUALITY_SYNCED,
        .payload_len = sizeof(v),
    };
    memcpy(tuple_vitals.mac, mac, 6);
    memcpy(tuple_vitals.payload, &v, sizeof(v));

    reset_mock_records();
    cloud_uplink_process_telemetry(&uplink, &tuple_vitals, 10600ULL);
    assert(s_record_count == 0); /* DROPPED! */

    /* Step 3: Broker ACKs CONNECT */
    cloud_mqtt_evt_t evt_ack_connect = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = connect_msg_id };
    cloud_uplink_post_mqtt_event(&uplink, &evt_ack_connect);
    cloud_uplink_process_events(&uplink);

    /* Assert: ATTRIBUTES is now published! */
    assert(s_record_count == 1);
    assert(strcmp(s_records[0].topic, "v1/gateway/attributes") == 0);
    int attr_msg_id = s_records[0].msg_id;

    /* Node is STILL waiting for attr ACK, so not active */
    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 0, 42) == false);

    /* Telemetry arrives while waiting for ATTR ACK -> STILL dropped */
    reset_mock_records();
    cloud_uplink_process_telemetry(&uplink, &tuple_vitals, 10700ULL);
    assert(s_record_count == 0); /* DROPPED! */

    /* Step 4: Broker ACKs ATTRIBUTES */
    cloud_mqtt_evt_t evt_ack_attr = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = attr_msg_id };
    cloud_uplink_post_mqtt_event(&uplink, &evt_ack_attr);
    cloud_uplink_process_events(&uplink);

    /* Assert: Node is now ACTIVE! */
    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 0, 42) == true);

    /* Step 5: Telemetry arrives -> MUST BE PUBLISHED! */
    reset_mock_records();
    tuple_vitals.timestamp_utc_ms = 10800ULL;
    cloud_uplink_process_telemetry(&uplink, &tuple_vitals, 10900ULL);
    assert(s_record_count == 1);
    assert(strcmp(s_records[0].topic, "v1/gateway/telemetry") == 0);

    printf("      -> PASS: Announce barrier strictly enforced; telemetry gated until Active\n");
}

static void test_ttl_expiration_dropped(void) {
    printf("[RUN] Test 2: Telemetry TTL expiration (>= 5000ms)...\n");
    cloud_uplink_t uplink;
    cloud_uplink_init(&uplink);

    cloud_mqtt_evt_t evt_conn = { .type = CLOUD_MQTT_EVT_CONNECTED, .msg_id = 0 };
    cloud_uplink_post_mqtt_event(&uplink, &evt_conn);
    cloud_uplink_process_events(&uplink);

    uint8_t mac[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    frame_snapshot_tuple_t tuple_conn = { .node_id = 1, .stream_type = STREAM_TYPE_STATUS, .session_id = 10 };
    memcpy(tuple_conn.mac, mac, 6);

    reset_mock_records();
    cloud_uplink_handle_node_status(&uplink, &tuple_conn, true);
    int c_id = s_records[0].msg_id;

    cloud_mqtt_evt_t e1 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = c_id };
    cloud_uplink_post_mqtt_event(&uplink, &e1);
    cloud_uplink_process_events(&uplink);

    int a_id = s_records[1].msg_id;
    cloud_mqtt_evt_t e2 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = a_id };
    cloud_uplink_post_mqtt_event(&uplink, &e2);
    cloud_uplink_process_events(&uplink);

    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 1, 10) == true);

    /* Freshness check at 4999ms -> PASS */
    reset_mock_records();
    vitals_packet_t v = { .hr_ecg = 70, .hr_ppg = 70, .spo2 = 99, .temp_x10 = 366 };
    frame_snapshot_tuple_t tuple_v = {
        .node_id = 1, .stream_type = STREAM_TYPE_VITALS, .session_id = 10,
        .timestamp_utc_ms = 10000ULL, .time_quality = TIME_QUALITY_SYNCED, .payload_len = sizeof(v)
    };
    memcpy(tuple_v.mac, mac, 6);
    memcpy(tuple_v.payload, &v, sizeof(v));

    cloud_uplink_process_telemetry(&uplink, &tuple_v, 14999ULL);
    assert(s_record_count == 1);

    /* Freshness check at 5001ms (expired) -> DROPPED */
    reset_mock_records();
    cloud_uplink_process_telemetry(&uplink, &tuple_v, 15001ULL);
    assert(s_record_count == 0);

    printf("      -> PASS: TTL expiration correctly drops stale telemetry\n");
}

static void test_disconnect_and_stale_ack(void) {
    printf("[RUN] Test 3: Disconnect revokes Active & duplicate ACK handling...\n");
    cloud_uplink_t uplink;
    cloud_uplink_init(&uplink);

    cloud_mqtt_evt_t evt_conn = { .type = CLOUD_MQTT_EVT_CONNECTED, .msg_id = 0 };
    cloud_uplink_post_mqtt_event(&uplink, &evt_conn);
    cloud_uplink_process_events(&uplink);

    uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    frame_snapshot_tuple_t tuple = { .node_id = 2, .stream_type = STREAM_TYPE_STATUS, .session_id = 88 };
    memcpy(tuple.mac, mac, 6);

    reset_mock_records();
    cloud_uplink_handle_node_status(&uplink, &tuple, true);
    int c_id = s_records[0].msg_id;

    cloud_mqtt_evt_t e1 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = c_id };
    cloud_uplink_post_mqtt_event(&uplink, &e1);
    cloud_uplink_process_events(&uplink);

    int a_id = s_records[1].msg_id;
    cloud_mqtt_evt_t e2 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = a_id };
    cloud_uplink_post_mqtt_event(&uplink, &e2);
    cloud_uplink_process_events(&uplink);

    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 2, 88) == true);

    /* Duplicate ACK for a_id does NOT re-publish attributes */
    reset_mock_records();
    cloud_uplink_post_mqtt_event(&uplink, &e2);
    cloud_uplink_process_events(&uplink);
    assert(s_record_count == 0);

    /* Disconnect node */
    reset_mock_records();
    cloud_uplink_handle_node_status(&uplink, &tuple, false);
    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 2, 88) == false);
    assert(s_record_count == 2); /* 1 attr (bleConnected=false) + 1 disconnect */

    /* Telemetry after disconnect -> DROPPED */
    reset_mock_records();
    vitals_packet_t v = { .hr_ecg = 80, .hr_ppg = 80, .spo2 = 98, .temp_x10 = 368 };
    frame_snapshot_tuple_t tuple_v = {
        .node_id = 2, .stream_type = STREAM_TYPE_VITALS, .session_id = 88,
        .timestamp_utc_ms = 20000ULL, .time_quality = TIME_QUALITY_SYNCED, .payload_len = sizeof(v)
    };
    memcpy(tuple_v.mac, mac, 6);
    memcpy(tuple_v.payload, &v, sizeof(v));

    cloud_uplink_process_telemetry(&uplink, &tuple_v, 20100ULL);
    assert(s_record_count == 0);

    printf("      -> PASS: Disconnect latches offline and duplicate ACK is ignored\n");
}

static void test_pre_sntp_and_invalid_time_drop(void) {
    printf("[RUN] Test 4: Pre-SNTP (UNKNOWN) and INVALID time dropped from live publish...\n");
    cloud_uplink_t uplink;
    cloud_uplink_init(&uplink);

    /* Broker connects */
    cloud_mqtt_evt_t evt_conn = { .type = CLOUD_MQTT_EVT_CONNECTED, .msg_id = 0 };
    cloud_uplink_post_mqtt_event(&uplink, &evt_conn);
    cloud_uplink_process_events(&uplink);

    reset_mock_records();

    /* Make slot 3 active */
    uint8_t mac[6] = { 0xAA, 0x11, 0x22, 0x33, 0x44, 0x55 };
    frame_snapshot_tuple_t status_tuple = {
        .node_id = 3, .session_id = 77, .bridge_epoch = 10, .stream_type = STREAM_TYPE_STATUS
    };
    memcpy(status_tuple.mac, mac, 6);
    status_tuple.payload[0] = 1;
    status_tuple.payload_len = 1;

    cloud_uplink_handle_node_status(&uplink, &status_tuple, true);
    assert(s_record_count == 1);
    int connect_msg_id = s_records[0].msg_id;

    /* Advance through announce handshake: ACK connect */
    cloud_mqtt_evt_t evt1 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = connect_msg_id };
    cloud_uplink_post_mqtt_event(&uplink, &evt1);
    cloud_uplink_process_events(&uplink);

    assert(s_record_count == 2);
    int attr_msg_id = s_records[1].msg_id;

    /* ACK attr */
    cloud_mqtt_evt_t evt2 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = attr_msg_id };
    cloud_uplink_post_mqtt_event(&uplink, &evt2);
    cloud_uplink_process_events(&uplink);

    assert(mqtt_lifecycle_is_node_active(&uplink.lifecycle, 3, 77) == true);

    /* 1. Tuple with timestamp_utc_ms == 0 and TIME_QUALITY_UNKNOWN -> MUST DROP */
    vitals_packet_t v = { .hr_ecg = 75, .hr_ppg = 74, .spo2 = 98, .temp_x10 = 365 };
    frame_snapshot_tuple_t tuple_unsynced = {
        .node_id = 3, .stream_type = STREAM_TYPE_VITALS, .session_id = 77,
        .timestamp_utc_ms = 0, .time_quality = TIME_QUALITY_UNKNOWN, .payload_len = sizeof(v)
    };
    memcpy(tuple_unsynced.mac, mac, 6);
    memcpy(tuple_unsynced.payload, &v, sizeof(v));

    reset_mock_records();
    cloud_uplink_process_telemetry(&uplink, &tuple_unsynced, 10000ULL);
    assert(s_record_count == 0);

    /* 2. Tuple with TIME_QUALITY_INVALID -> MUST DROP */
    frame_snapshot_tuple_t tuple_invalid = tuple_unsynced;
    tuple_invalid.timestamp_utc_ms = 9999ULL;
    tuple_invalid.time_quality = TIME_QUALITY_INVALID;

    cloud_uplink_process_telemetry(&uplink, &tuple_invalid, 10000ULL);
    assert(s_record_count == 0);

    printf("      -> PASS: Pre-SNTP and invalid time tuples cleanly blocked from publishing\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING CLOUD UPLINK INTEGRATION TESTS (TDD)     \n");
    printf("====================================================\n\n");

    test_announce_barrier_and_telemetry_gate();
    test_ttl_expiration_dropped();
    test_disconnect_and_stale_ack();
    test_pre_sntp_and_invalid_time_drop();

    printf("\n🎉 ALL CLOUD UPLINK INTEGRATION TESTS PASSED!\n");
    return 0;
}
