/**
 * @file test_mqtt_lifecycle.c
 * @brief Host-based Unit Tests for MQTT Lifecycle & PUBACK Tracking (TDD)
 *
 * Verifies Architecture v2 Contract §5 & Task T09:
 * 1. CONNECT QoS 1 -> PUBACK tracking (waiting for ACK before active).
 * 2. Connect ACK triggers ATTRIBUTES QoS 1 -> PUBACK confirmation -> ACTIVE state.
 * 3. Stale ACK, wrong generation, or unknown message-id rejected (no premature active).
 * 4. Telemetry blocked until node is ACTIVE with matching session.
 * 5. Outbox TTL: frames older than 5000ms rejected from publish.
 * 6. Broker disconnect/reconnect bumps generation, resets announced state, and drops old ACKs.
 * 7. Peripheral disconnect while MQTT offline prevents replay upon reconnect.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "thingsboard_mqtt.h"

static void test_connect_puback_attributes_sequence(void) {
    printf("[RUN] Test 1: Connect QoS 1 -> PUBACK -> Attributes -> PUBACK -> Active...\n");
    mqtt_lifecycle_mgr_t mgr;
    mqtt_lifecycle_init(&mgr);
    mqtt_lifecycle_on_broker_connected(&mgr);

    uint8_t mac[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    uint8_t slot_id = 0;
    uint32_t session_id = 42;

    /* 1. Node connects: Publish CONNECT with msg_id=100 */
    int ret = mqtt_lifecycle_start_announce(&mgr, slot_id, mac, session_id, 100);
    assert(ret == 0);
    assert(mgr.nodes[slot_id].state == NODE_MQTT_STATE_WAIT_CONNECT_ACK);

    /* Telemetry must NOT be allowed yet */
    bool can_publish = mqtt_lifecycle_can_publish_telemetry(&mgr, slot_id, session_id, 1000, 1100, 5000);
    assert(can_publish == false);

    /* 2. PUBACK for msg_id=100 arrives */
    uint8_t ack_slot = 0xFF;
    bool ready_for_attr = false;
    ret = mqtt_lifecycle_handle_puback(&mgr, 100, &ack_slot, &ready_for_attr);
    assert(ret == 0);
    assert(ack_slot == slot_id);
    assert(ready_for_attr == true);
    assert(mgr.nodes[slot_id].state == NODE_MQTT_STATE_WAIT_ATTR_ACK);

    /* 3. Publish ATTRIBUTES with msg_id=101 */
    ret = mqtt_lifecycle_set_attributes_msg_id(&mgr, slot_id, 101);
    assert(ret == 0);

    /* Still not active until attributes ACKed */
    assert(mqtt_lifecycle_is_node_active(&mgr, slot_id, session_id) == false);

    /* 4. PUBACK for msg_id=101 arrives */
    ready_for_attr = false;
    ret = mqtt_lifecycle_handle_puback(&mgr, 101, &ack_slot, &ready_for_attr);
    assert(ret == 0);
    assert(ack_slot == slot_id);
    assert(ready_for_attr == false); /* Already announced attributes */
    assert(mgr.nodes[slot_id].state == NODE_MQTT_STATE_ACTIVE);
    assert(mqtt_lifecycle_is_node_active(&mgr, slot_id, session_id) == true);

    /* 5. Telemetry is now allowed */
    can_publish = mqtt_lifecycle_can_publish_telemetry(&mgr, slot_id, session_id, 1000, 1100, 5000);
    assert(can_publish == true);

    printf("      -> PASS: Full handshake completed: CONNECT -> ACK -> ATTR -> ACK -> ACTIVE\n");
}

static void test_stale_ack_and_generation_barrier(void) {
    printf("[RUN] Test 2: Stale ACK, wrong generation, and reconnect barrier...\n");
    mqtt_lifecycle_mgr_t mgr;
    mqtt_lifecycle_init(&mgr);
    mqtt_lifecycle_on_broker_connected(&mgr);

    uint8_t mac[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    mqtt_lifecycle_start_announce(&mgr, 0, mac, 10, 200);

    /* Random/unknown message ID */
    uint8_t ack_slot = 0xFF;
    bool ready_for_attr = false;
    int ret = mqtt_lifecycle_handle_puback(&mgr, 999, &ack_slot, &ready_for_attr);
    assert(ret != 0);
    assert(mgr.stale_ack_drops == 1);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_WAIT_CONNECT_ACK);

    /* Broker disconnects and reconnects (generation incremented) */
    mqtt_lifecycle_on_broker_disconnected(&mgr);
    assert(mgr.mqtt_connected == false);
    mqtt_lifecycle_on_broker_connected(&mgr);
    assert(mgr.mqtt_connected == true);
    assert(mgr.mqtt_generation == 2);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_IDLE);

    /* Late arriving ACK from previous generation (msg_id=200) is rejected */
    ret = mqtt_lifecycle_handle_puback(&mgr, 200, &ack_slot, &ready_for_attr);
    assert(ret != 0);
    assert(mgr.stale_ack_drops == 2);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_IDLE);

    printf("      -> PASS: Stale ACKs and pre-reconnect ACKs rejected by generation barrier\n");
}

static void test_outbox_ttl_expiration(void) {
    printf("[RUN] Test 3: Outbox TTL expiration (5000ms window)...\n");
    mqtt_lifecycle_mgr_t mgr;
    mqtt_lifecycle_init(&mgr);
    mqtt_lifecycle_on_broker_connected(&mgr);

    uint8_t mac[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    mqtt_lifecycle_start_announce(&mgr, 0, mac, 1, 300);
    uint8_t ack_slot;
    bool ready;
    mqtt_lifecycle_handle_puback(&mgr, 300, &ack_slot, &ready);
    mqtt_lifecycle_set_attributes_msg_id(&mgr, 0, 301);
    mqtt_lifecycle_handle_puback(&mgr, 301, &ack_slot, &ready);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_ACTIVE);

    /* Case A: Fresh frame (age = 2000ms < 5000ms) -> Allowed */
    assert(mqtt_lifecycle_can_publish_telemetry(&mgr, 0, 1, 10000, 12000, 5000) == true);

    /* Case B: Boundary frame (age = 4999ms < 5000ms) -> Allowed */
    assert(mqtt_lifecycle_can_publish_telemetry(&mgr, 0, 1, 10000, 14999, 5000) == true);

    /* Case C: Expired frame (age = 5001ms >= 5000ms) -> Dropped */
    assert(mqtt_lifecycle_can_publish_telemetry(&mgr, 0, 1, 10000, 15001, 5000) == false);
    assert(mgr.ttl_dropped_count == 1);

    /* Case D: Clock jitter tolerance (event is 50ms ahead of now <= 2000ms) -> Allowed */
    assert(mqtt_lifecycle_can_publish_telemetry(&mgr, 0, 1, 10050, 10000, 5000) == true);

    /* Case E: Excessive future timestamp (> 2000ms) -> Dropped */
    assert(mqtt_lifecycle_can_publish_telemetry(&mgr, 0, 1, 12001, 10000, 5000) == false);

    printf("      -> PASS: Frames aged > 5000ms cleanly dropped without reaching MQTT publish\n");
}

static void test_offline_ble_disconnect_isolation(void) {
    printf("[RUN] Test 4: Peripheral disconnect while MQTT is offline...\n");
    mqtt_lifecycle_mgr_t mgr;
    mqtt_lifecycle_init(&mgr);
    mqtt_lifecycle_on_broker_connected(&mgr);

    uint8_t mac[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    mqtt_lifecycle_start_announce(&mgr, 1, mac, 99, 400);
    uint8_t ack_slot;
    bool ready;
    mqtt_lifecycle_handle_puback(&mgr, 400, &ack_slot, &ready);
    mqtt_lifecycle_set_attributes_msg_id(&mgr, 1, 401);
    mqtt_lifecycle_handle_puback(&mgr, 401, &ack_slot, &ready);
    assert(mgr.nodes[1].state == NODE_MQTT_STATE_ACTIVE);

    /* Network drop */
    mqtt_lifecycle_on_broker_disconnected(&mgr);

    /* While network is offline, BLE peripheral disconnects */
    mqtt_lifecycle_handle_node_disconnect(&mgr, 1);
    assert(mgr.nodes[1].state == NODE_MQTT_STATE_IDLE);
    assert(mgr.nodes[1].session_id == 0);

    /* Network reconnects */
    mqtt_lifecycle_on_broker_connected(&mgr);

    /* Disconnected node remains IDLE and cannot publish */
    assert(mqtt_lifecycle_is_node_active(&mgr, 1, 99) == false);
    assert(mqtt_lifecycle_can_publish_telemetry(&mgr, 1, 99, 1000, 1100, 5000) == false);

    printf("      -> PASS: Disconnected peripheral while offline not falsely re-announced\n");
}

static void test_announce_timeout_watchdog(void) {
    printf("[RUN] Test 5: Announce handshake timeout watchdog (5000ms reset)...\n");
    mqtt_lifecycle_mgr_t mgr;
    mqtt_lifecycle_init(&mgr);
    mqtt_lifecycle_on_broker_connected(&mgr);

    uint8_t mac[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    mqtt_lifecycle_start_announce(&mgr, 0, mac, 123, 500);
    mqtt_lifecycle_set_announce_time(&mgr, 0, 1000);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_WAIT_CONNECT_ACK);

    /* Time advances to 3000ms (elapsed = 2000ms < 5000ms) -> Still waiting */
    mqtt_lifecycle_check_timeouts(&mgr, 3000, 5000);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_WAIT_CONNECT_ACK);

    /* Time advances to 6001ms (elapsed = 5001ms >= 5000ms) -> Handshake timed out, reset to IDLE */
    mqtt_lifecycle_check_timeouts(&mgr, 6001, 5000);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_IDLE);
    assert(mgr.nodes[0].connect_msg_id == 0);
    assert(mgr.nodes[0].attr_msg_id == 0);
    assert(mgr.nodes[0].announce_start_mono_ms == 0);

    /* Can now start a fresh announce */
    int ret = mqtt_lifecycle_start_announce(&mgr, 0, mac, 123, 501);
    assert(ret == 0);
    assert(mgr.nodes[0].state == NODE_MQTT_STATE_WAIT_CONNECT_ACK);

    printf("      -> PASS: Stuck announce reset to IDLE after 5000ms timeout for clean recovery\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING MQTT LIFECYCLE & PUBACK TESTS (TDD)      \n");
    printf("====================================================\n\n");

    test_connect_puback_attributes_sequence();
    test_stale_ack_and_generation_barrier();
    test_outbox_ttl_expiration();
    test_offline_ble_disconnect_isolation();
    test_announce_timeout_watchdog();

    printf("\n🎉 ALL MQTT LIFECYCLE TESTS PASSED!\n");
    return 0;
}
