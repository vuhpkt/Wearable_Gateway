/**
 * @file test_gateway_downlink_pipeline.c
 * @brief Host Integration Test: End-to-End Gateway Downlink Pipeline Simulation
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1
#define HOST_TEST 1

#include "ble_protocol_def.h"
#include "uart_frame_def.h"
#include "downlink_synthesizer.h"
#include "cloud_uplink.h"
#include "nrf_central/src/uart_tx_bridge.h"

/* Simulated Central BLE State */
static uint8_t s_ble_received_slot = 0xFF;
static uint8_t s_ble_received_cmd = 0;
static uint16_t s_ble_received_val = 0;
static uint32_t s_ble_write_count = 0;

/* Mock UART Bridge TX Raw */
static uint8_t s_mock_uart_tx_buf[128];
static size_t s_mock_uart_tx_len = 0;
static uint32_t s_mock_uart_tx_count = 0;

int uart_bridge_tx_raw(const uint8_t *frame_data, size_t len) {
    if (!frame_data || len == 0) return -1;
    s_mock_uart_tx_len = len;
    memcpy(s_mock_uart_tx_buf, frame_data, len < sizeof(s_mock_uart_tx_buf) ? len : sizeof(s_mock_uart_tx_buf));
    s_mock_uart_tx_count++;
    return (int)len;
}

int uart_bridge_trigger_snapshot_sync(void) {
    return 0;
}

bool tb_mqtt_is_connected(void) {
    return true;
}

static uint32_t s_mock_mqtt_pub_count = 0;
int tb_mqtt_publish(const char *topic, const char *json_payload, int qos) {
    (void)topic;
    (void)json_payload;
    (void)qos;
    s_mock_mqtt_pub_count++;
    return 1;
}

int ble_central_mgr_write_cmd(uint8_t slot_id, const uint8_t *data, uint16_t len) {
    s_ble_received_slot = slot_id;
    s_ble_received_cmd = data[0];
    if (data[0] == CMD_VITAL_CFG && len >= 3) {
        s_ble_received_val = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    }
    s_ble_write_count++;
    return 0;
}

int main(void) {
    printf("====================================================\n");
    printf(" RUNNING FULL GATEWAY DOWNLINK PIPELINE INTEGRATION \n");
    printf("====================================================\n\n");

    /* 1. Khởi tạo ESP32 Cloud Uplink & Giả lập Node 3 đã kết nối với MAC E4:5F:01:23:45:67 */
    printf("[RUN] Step 1: Initialize Cloud Uplink and register active Node 3...\n");
    cloud_uplink_t uplink;
    cloud_uplink_init(&uplink);
    uplink.lifecycle.mqtt_connected = true;

    uint8_t target_mac[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    uint8_t target_slot = 3;
    uplink.lifecycle.nodes[target_slot].state = NODE_MQTT_STATE_ACTIVE;
    uplink.lifecycle.nodes[target_slot].session_id = 1001;
    memcpy(uplink.lifecycle.nodes[target_slot].mac, target_mac, 6);
    printf("      -> PASS: Uplink ready, Node 3 active with MAC E4:5F:01:23:45:67\n");

    /* 2. Payload nhận được từ ThingsBoard MQTT trên topic "v1/gateway/attributes" */
    printf("[RUN] Step 2: Ingest ThingsBoard MQTT JSON attributes payload...\n");
    const char *tb_mqtt_json = "{"
        "\"device\":\"E4:5F:01:23:45:67\","
        "\"data\":{\"vitalInterval\":2000}"
    "}";

    /* BƯỚC A: Xử lý qua cloud_uplink_handle_downlink_attributes */
    uint8_t uart_wire_buf[128];
    int wire_len = cloud_uplink_handle_downlink_attributes(&uplink, tb_mqtt_json, uart_wire_buf, sizeof(uart_wire_buf));
    assert(wire_len == 3 + UART_FRAME_OVERHEAD); // 3 + 5 = 8 bytes
    assert(uart_wire_buf[0] == UART_FRAME_SOF);
    assert(uart_wire_buf[1] == target_slot);
    assert(uart_wire_buf[2] == UART_PKT_TYPE_DOWNLINK);
    assert(uart_wire_buf[3] == 3);
    assert(uart_wire_buf[4] == CMD_VITAL_CFG);
    uint8_t calc_crc = uart_crc8_calc(&uart_wire_buf[1], 3 + 3);
    assert(uart_wire_buf[7] == calc_crc);
    printf("      -> PASS: Downlink attributes translated to UART Frame 0x80 for Slot %u\n", target_slot);

    /* BƯỚC B: Chuyển qua Central Dispatcher */
    printf("[RUN] Step 3: Forward UART frame to Central Dispatcher...\n");
    uint8_t rx_slot = uart_wire_buf[1];
    uint8_t rx_len = uart_wire_buf[3];
    const uint8_t *rx_payload = &uart_wire_buf[4];

    int dispatch_ret = central_bridge_dispatch_downlink(rx_slot, rx_payload, rx_len);
    assert(dispatch_ret == 0);
    assert(s_ble_write_count == 1);
    assert(s_ble_received_slot == target_slot);
    assert(s_ble_received_cmd == CMD_VITAL_CFG);
    assert(s_ble_received_val == 2000);
    printf("      -> PASS: Central Dispatcher successfully forwarded command to BLE layer!\n");

    /* BƯỚC C: Kịch bản rớt gói an toàn khi target node không kết nối */
    printf("[RUN] Step 4: Verify graceful drop when device MAC is not connected...\n");
    const char *tb_unconnected_json = "{"
        "\"device\":\"11:22:33:44:55:66\","
        "\"data\":{\"vitalInterval\":2000}"
    "}";
    int drop_len = cloud_uplink_handle_downlink_attributes(&uplink, tb_unconnected_json, uart_wire_buf, sizeof(uart_wire_buf));
    assert(drop_len == -4); // Target node not connected
    assert(s_ble_write_count == 1); // Counter did not increase
    printf("      -> PASS: Unconnected device safely dropped with code -4\n");

    /* BƯỚC C2: Verify downlink resolution succeeds while in-flight handshake (WAIT_CONNECT_ACK / WAIT_ATTR_ACK) */
    printf("[RUN] Step 4b: Verify downlink routing succeeds during in-flight MQTT handshake...\n");
    uplink.lifecycle.nodes[target_slot].state = NODE_MQTT_STATE_WAIT_CONNECT_ACK;
    assert(cloud_uplink_find_slot_by_mac(&uplink, target_mac) == (int)target_slot);
    uplink.lifecycle.nodes[target_slot].state = NODE_MQTT_STATE_WAIT_ATTR_ACK;
    assert(cloud_uplink_find_slot_by_mac(&uplink, target_mac) == (int)target_slot);
    uplink.lifecycle.nodes[target_slot].state = NODE_MQTT_STATE_ACTIVE;
    printf("      -> PASS: In-flight handshake states successfully resolve target slot!\n");

    /* BƯỚC D: Kiểm thử Ingest Callback cloud_uplink_on_mqtt_data & Gateway Gating */
    printf("[RUN] Step 5: Verify cloud_uplink_on_mqtt_data callback and UART raw TX...\n");
    const char *tb_ecg_off_json = "{"
        "\"device\":\"E4:5F:01:23:45:67\","
        "\"data\":{\"showEcg\":0}"
    "}";
    s_mock_uart_tx_count = 0;
    cloud_uplink_on_mqtt_data(&uplink, "v1/gateway/attributes", strlen("v1/gateway/attributes"),
                              tb_ecg_off_json, strlen(tb_ecg_off_json));
    assert(s_mock_uart_tx_count == 1);
    assert(s_mock_uart_tx_buf[0] == UART_FRAME_SOF);
    assert(s_mock_uart_tx_buf[1] == target_slot);
    assert(s_mock_uart_tx_buf[2] == UART_PKT_TYPE_DOWNLINK);
    assert(s_mock_uart_tx_buf[3] == 7); // CMD_MODE_CFG length
    assert(s_mock_uart_tx_buf[4] == CMD_MODE_CFG);
    assert(s_mock_uart_tx_buf[10] == 0); // ecgEnabled == 0
    assert(uplink.ecg_stream_enabled[target_slot] == false); // Gateway Gating asserted

    /* Test telemetry gating: STREAM_TYPE_ECG should be dropped when disabled */
    int16_t sample_data[50] = {100, 200, 300};
    frame_snapshot_tuple_t ecg_tuple = {
        .node_id = target_slot,
        .session_id = 1001,
        .stream_type = STREAM_TYPE_ECG,
        .time_quality = TIME_QUALITY_SYNCED,
        .payload_len = sizeof(sample_data),
        .timestamp_utc_ms = 1700000000000ULL,
    };
    memcpy(ecg_tuple.mac, target_mac, 6);
    memcpy(ecg_tuple.payload, sample_data, sizeof(sample_data));

    s_mock_mqtt_pub_count = 0;
    cloud_uplink_process_telemetry(&uplink, &ecg_tuple, 1700000000000ULL);
    assert(s_mock_mqtt_pub_count == 0); // Gated! No telemetry published
    printf("      -> PASS: ECG telemetry gated (0 MQTT publishes while disabled)\n");

    /* Turn ECG back ON */
    const char *tb_ecg_on_json = "{"
        "\"device\":\"E4:5F:01:23:45:67\","
        "\"data\":{\"showEcg\":1}"
    "}";
    cloud_uplink_on_mqtt_data(&uplink, "v1/gateway/attributes", strlen("v1/gateway/attributes"),
                              tb_ecg_on_json, strlen(tb_ecg_on_json));
    assert(uplink.ecg_stream_enabled[target_slot] == true);
    s_mock_mqtt_pub_count = 0;
    cloud_uplink_process_telemetry(&uplink, &ecg_tuple, 1700000000000ULL);
    assert(s_mock_mqtt_pub_count == 1); // Published!
    printf("      -> PASS: ECG telemetry re-enabled (1 MQTT publish delivered)\n");

    /* BƯỚC E: Kiểm thử Kịch bản Snapshot Burst 10 slot khi đang stream ECG (Ngăn chặn State Demotion) */
    printf("[RUN] Step 6: Ingest 10-slot Central snapshot burst during active streaming...\n");
    s_mock_mqtt_pub_count = 0;

    for (uint8_t slot = 0; slot < 10; slot++) {
        frame_snapshot_tuple_t status_tuple = {
            .node_id = slot,
            .stream_type = STREAM_TYPE_STATUS,
            .time_quality = TIME_QUALITY_SYNCED,
            .timestamp_utc_ms = 1700000000000ULL,
            .esp_rx_mono_ms = 2000,
        };
        if (slot == target_slot) {
            status_tuple.session_id = 1001;
            status_tuple.payload[0] = 1; // Connected
            memcpy(status_tuple.mac, target_mac, 6);
        } else {
            status_tuple.session_id = 0;
            status_tuple.payload[0] = 0; // Disconnected
            memset(status_tuple.mac, 0, 6);
        }
        cloud_uplink_dispatch_tuple(&uplink, &status_tuple);
    }

    /* Kiểm tra Node 3 vẫn ở trạng thái ACTIVE (không bị demote về WAIT_CONNECT_ACK = 1) */
    assert(uplink.lifecycle.nodes[target_slot].state == NODE_MQTT_STATE_ACTIVE);
    assert(uplink.lifecycle.nodes[target_slot].session_id == 1001);

    /* Không được gửi broadcast disconnect cho các slot trống MAC 00:00:00:00:00:00 */
    assert(s_mock_mqtt_pub_count == 0);

    /* Stream tiếp một gói ECG batch -> Phải được publish ngay lập tức, không bị Inactive drop */
    s_mock_mqtt_pub_count = 0;
    cloud_uplink_dispatch_tuple(&uplink, &ecg_tuple);
    assert(s_mock_mqtt_pub_count == 1); // ECG published to v1/gateway/telemetry!
    /* BƯỚC F: Kiểm thử Bảo toàn Trạng thái ECG khi Downlink chỉ cấu hình Mode/Interval */
    printf("[RUN] Step 7: Verify ECG gating is preserved when downlink updates other fields...\n");
    /* 1. Toggle ECG OFF */
    const char *tb_ecg_off_json2 = "{\"device\":\"E4:5F:01:23:45:67\",\"data\":{\"showEcg\":0}}";
    cloud_uplink_on_mqtt_data(&uplink, "v1/gateway/attributes", strlen("v1/gateway/attributes"),
                              tb_ecg_off_json2, strlen(tb_ecg_off_json2));
    assert(uplink.ecg_stream_enabled[target_slot] == false);

    /* 2. Gửi downlink chỉ thay đổi periodicInterval (không có showEcg hay ecgEnabled) */
    const char *tb_interval_only = "{\"device\":\"E4:5F:01:23:45:67\",\"data\":{\"periodicInterval\":45}}";
    s_mock_uart_tx_count = 0;
    cloud_uplink_on_mqtt_data(&uplink, "v1/gateway/attributes", strlen("v1/gateway/attributes"),
                              tb_interval_only, strlen(tb_interval_only));
    assert(s_mock_uart_tx_count == 1);
    assert(uplink.ecg_stream_enabled[target_slot] == false); // Trạng thái gating KHÔNG bị ghi đè thành true!
    assert(s_mock_uart_tx_buf[10] == 0); // Byte ecgEnabled trong CMD_MODE_CFG bảo toàn giá trị 0

    /* 3. Đảm bảo ECG telemetry tiếp tục bị gate */
    s_mock_mqtt_pub_count = 0;
    cloud_uplink_process_telemetry(&uplink, &ecg_tuple, 1700000000000ULL);
    assert(s_mock_mqtt_pub_count == 0);
    printf("      -> PASS: ECG gating state safely preserved when downlink payload omits showEcg\n");

    /* BƯỚC G: Kiểm thử Ngắt Kết nối Snapshot Zero-MAC & Tái kích hoạt ECG trên Session Mới */
    printf("[RUN] Step 8: Verify Zero-MAC snapshot disconnect and fresh session ECG reset...\n");
    /* 1. Central gửi snapshot slot 3 trống (status = 0, MAC = 00:00:00:00:00:00) */
    frame_snapshot_tuple_t zero_mac_status = {
        .node_id = target_slot,
        .stream_type = STREAM_TYPE_STATUS,
        .session_id = 0,
        .time_quality = TIME_QUALITY_SYNCED,
        .timestamp_utc_ms = 1700000000000ULL,
        .esp_rx_mono_ms = 3000,
    };
    zero_mac_status.payload[0] = 0; // Disconnected
    memset(zero_mac_status.mac, 0, 6);

    s_mock_mqtt_pub_count = 0;
    cloud_uplink_dispatch_tuple(&uplink, &zero_mac_status);
    /* Node 3 phải chuyển từ ACTIVE sang IDLE */
    assert(uplink.lifecycle.nodes[target_slot].state == NODE_MQTT_STATE_IDLE);
    /* Gating ECG phải được reset về true (mặc định cho thiết bị/bệnh nhân tiếp theo) */
    assert(uplink.ecg_stream_enabled[target_slot] == true);
    /* Phải phát 1 gói attributes offline + 1 gói disconnect */
    assert(s_mock_mqtt_pub_count == 2);

    /* 2. Snapshot tiếp theo với MAC 00:00:00:00:00:00 trên slot đã IDLE phải được bỏ qua hoàn toàn */
    s_mock_mqtt_pub_count = 0;
    cloud_uplink_dispatch_tuple(&uplink, &zero_mac_status);
    assert(s_mock_mqtt_pub_count == 0); // Idempotent! Không có publish rác

    /* 3. Node 3 kết nối lại trong session mới (session_id = 1002) */
    frame_snapshot_tuple_t conn_tuple = {
        .node_id = target_slot,
        .stream_type = STREAM_TYPE_STATUS,
        .session_id = 1002,
        .time_quality = TIME_QUALITY_SYNCED,
        .timestamp_utc_ms = 1700000005000ULL,
        .esp_rx_mono_ms = 3500,
    };
    conn_tuple.payload[0] = 1; // Connected
    memcpy(conn_tuple.mac, target_mac, 6);

    s_mock_mqtt_pub_count = 0;
    cloud_uplink_dispatch_tuple(&uplink, &conn_tuple);
    assert(s_mock_mqtt_pub_count == 1); // Connect publish
    assert(uplink.lifecycle.nodes[target_slot].state == NODE_MQTT_STATE_WAIT_CONNECT_ACK);

    /* Giả lập PUBACK cho connect -> Kích hoạt publish attributes */
    cloud_mqtt_evt_t puback1 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = 1 };
    cloud_uplink_post_mqtt_event(&uplink, &puback1);
    cloud_uplink_process_events(&uplink);
    assert(uplink.lifecycle.nodes[target_slot].state == NODE_MQTT_STATE_WAIT_ATTR_ACK);
    assert(s_mock_mqtt_pub_count == 2); // Attributes published

    /* Giả lập PUBACK cho attributes -> Node chuyển sang ACTIVE */
    cloud_mqtt_evt_t puback2 = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = 1 };
    cloud_uplink_post_mqtt_event(&uplink, &puback2);
    cloud_uplink_process_events(&uplink);
    assert(uplink.lifecycle.nodes[target_slot].state == NODE_MQTT_STATE_ACTIVE);

    /* 4. Stream ECG trên session mới -> Phải publish thành công vì ecg_stream_enabled được reset */
    ecg_tuple.session_id = 1002;
    ecg_tuple.timestamp_utc_ms = 1700000006000ULL;
    s_mock_mqtt_pub_count = 0;
    cloud_uplink_process_telemetry(&uplink, &ecg_tuple, 1700000006000ULL);
    assert(s_mock_mqtt_pub_count == 1); // Published!
    printf("      -> PASS: Zero-MAC snapshot disconnect latched offline; new session reset ECG gating to enabled!\n");

    printf("\n🎉 ALL GATEWAY PIPELINE INTEGRATION TESTS PASSED!\n");
    return 0;
}
