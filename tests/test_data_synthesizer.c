/**
 * @file test_data_synthesizer.c
 * @brief Host-based Unit Tests for ThingsBoard JSON Data Synthesizer (TDD)
 *
 * Verifies Architecture v2 Contract §5 & Task T08:
 * 1. Legacy Node Connect/Disconnect & Vitals/ECG.
 * 2. MAC-Native Connect & Disconnect format and topics.
 * 3. MAC-Native CLIENT_SCOPE attributes (macAddress, bleConnected, bridgeEpoch hex, sessionId, statusSeq, statusTs).
 * 4. MAC-Native Vitals telemetry (observed ts, session metadata, no fallback server time).
 * 5. MAC-Native ECG batch telemetry (ecg_batch string, samplePeriodMs, sampleCount, batchEndTs).
 * 6. Bounds protection & truncation rejection.
 */

/* Standard C Libraries */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

/* Project Under Test */
#include "ble_protocol_def.h"
#include "bridge_time.h"
#include "frame_snapshot.h"
#include "data_synthesizer.h"

int main(void) {
    printf("====================================================\n");
    printf("    RUNNING DATA SYNTHESIZER UNIT TESTS (TDD)       \n");
    printf("====================================================\n\n");

    char buffer[2048];

    /* ─── TEST 1: Node Connect & Disconnect Status (Legacy) ─── */
    printf("[RUN] Test 1: Verify legacy Node connect & disconnect formatting...\n");
    const char *topic = NULL;
    int len = synth_format_node_status(buffer, sizeof(buffer), 0, true, &topic);
    assert(len > 0);
    assert(strcmp(topic, "v1/gateway/connect") == 0);
    assert(strcmp(buffer, "{\"device\":\"Node1\",\"type\":\"default\"}") == 0);

    topic = NULL;
    len = synth_format_node_status(buffer, sizeof(buffer), 9, false, &topic);
    assert(len > 0);
    assert(strcmp(topic, "v1/gateway/disconnect") == 0);
    assert(strcmp(buffer, "{\"device\":\"Node10\"}") == 0);
    printf("      -> PASS: Node1 connect & Node10 disconnect serialized correctly\n");

    /* ─── TEST 2: Full Vitals Serialization (Legacy) ─── */
    printf("[RUN] Test 2: Verify full vitals (HR ECG/PPG, SpO2, Temp)...\n");
    vitals_packet_t v_full = {
        .hr_ecg = 72,
        .hr_ppg = 70,
        .spo2 = 98,
        .temp_x10 = 365 /* 36.5 C */
    };
    len = synth_format_vitals(buffer, sizeof(buffer), 0, &v_full);
    assert(len > 0);
    assert(strstr(buffer, "\"Node1\"") != NULL);
    assert(strstr(buffer, "\"ecgHeartRate\":72") != NULL);
    assert(strstr(buffer, "\"ppgHeartRate\":70") != NULL);
    assert(strstr(buffer, "\"spo2\":98") != NULL);
    assert(strstr(buffer, "\"temperature\":36.5") != NULL);
    printf("      -> PASS: Full vitals: %s\n", buffer);

    /* ─── TEST 3: Partial Vitals Serialization (Legacy) ─── */
    printf("[RUN] Test 3: Verify partial vitals suppression (no zero fields)...\n");
    vitals_packet_t v_partial = {
        .hr_ecg = 0,
        .hr_ppg = 68,
        .spo2 = 0,
        .temp_x10 = 370 /* 37.0 C */
    };
    len = synth_format_vitals(buffer, sizeof(buffer), 2, &v_partial);
    assert(len > 0);
    assert(strstr(buffer, "\"Node3\"") != NULL);
    assert(strstr(buffer, "\"ppgHeartRate\":68") != NULL);
    assert(strstr(buffer, "\"temperature\":37.0") != NULL);
    assert(strstr(buffer, "ecgHeartRate") == NULL);
    assert(strstr(buffer, "spo2") == NULL);
    printf("      -> PASS: Partial vitals correctly excludes zero metrics\n");

    /* ─── TEST 4: Zero Vitals Handling ─── */
    printf("[RUN] Test 4: Verify zero vitals returns 0 (drop empty packet)...\n");
    vitals_packet_t v_zero = { .hr_ecg = 0, .hr_ppg = 0, .spo2 = 0, .temp_x10 = 0, .battery = 0 };
    len = synth_format_vitals(buffer, sizeof(buffer), 0, &v_zero);
    assert(len == 0);
    printf("      -> PASS: All-zero vitals returns 0, saving network bandwidth\n");

    /* ─── TEST 4b: Battery Vitals Serialization ─── */
    printf("[RUN] Test 4b: Verify battery serialization...\n");
    vitals_packet_t v_bat = {
        .hr_ecg = 0,
        .hr_ppg = 75,
        .spo2 = 98,
        .temp_x10 = 365,
        .battery = 85
    };
    len = synth_format_vitals(buffer, sizeof(buffer), 0, &v_bat);
    assert(len > 0);
    assert(strstr(buffer, "\"battery\":85") != NULL);
    printf("      -> PASS: Battery correctly included in vitals telemetry\n");

    /* ─── TEST 5: ECG Batch Serialization (Legacy) ─── */
    printf("[RUN] Test 5: Verify 50-sample ECG batch serialization...\n");
    int16_t ecg_samples[50];
    for (int i = 0; i < 50; i++) {
        ecg_samples[i] = (int16_t)(500 + i * 10);
    }
    len = synth_format_ecg_batch(buffer, sizeof(buffer), 1, ecg_samples, 50);
    assert(len > 0);
    assert(strstr(buffer, "\"Node2\"") != NULL);
    assert(strstr(buffer, "\"ecg_batch\":\"[500,510,") != NULL);
    assert(strstr(buffer, ",990]\"}}]}") != NULL);
    printf("      -> PASS: 50-sample ECG batch formatted (Length: %d bytes)\n", len);

    /* ─── TEST 6: Defensive Boundary & Error Checks ─── */
    printf("[RUN] Test 6: Verify defensive boundaries (NULL ptrs, tiny buffers)...\n");
    assert(synth_format_node_status(NULL, 100, 0, true, &topic) < 0);
    assert(synth_format_vitals(NULL, sizeof(buffer), 0, &v_full) < 0);
    assert(synth_format_vitals(buffer, sizeof(buffer), 0, NULL) < 0);
    assert(synth_format_ecg_batch(NULL, sizeof(buffer), 0, ecg_samples, 50) < 0);
    assert(synth_format_ecg_batch(buffer, sizeof(buffer), 0, NULL, 50) < 0);
    assert(synth_format_ecg_batch(buffer, sizeof(buffer), 0, ecg_samples, 0) < 0);

    assert(synth_format_node_status(buffer, 10, 0, true, &topic) < 0);
    assert(synth_format_vitals(buffer, 30, 0, &v_full) < 0);
    assert(synth_format_ecg_batch(buffer, 64, 0, ecg_samples, 50) < 0);
    printf("      -> PASS: All legacy boundary and null pointer violations rejected safely\n");

    /* ─── TEST 7: MAC-Native Connect & Disconnect (Architecture v2) ─── */
    printf("[RUN] Test 7: Verify MAC-native connect & disconnect formatting...\n");
    uint8_t mac_test[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    topic = NULL;
    len = synth_format_mac_connect(buffer, sizeof(buffer), mac_test, &topic);
    assert(len > 0);
    assert(strcmp(topic, "v1/gateway/connect") == 0);
    assert(strcmp(buffer, "{\"device\":\"E4:5F:01:23:45:67\",\"type\":\"Wearable\"}") == 0);

    topic = NULL;
    len = synth_format_mac_disconnect(buffer, sizeof(buffer), mac_test, &topic);
    assert(len > 0);
    assert(strcmp(topic, "v1/gateway/disconnect") == 0);
    assert(strcmp(buffer, "{\"device\":\"E4:5F:01:23:45:67\"}") == 0);
    printf("      -> PASS: MAC-native connect and disconnect match Golden JSON\n");

    /* ─── TEST 8: MAC-Native CLIENT_SCOPE Attributes ─── */
    printf("[RUN] Test 8: Verify MAC-native CLIENT_SCOPE attributes...\n");
    frame_snapshot_tuple_t tuple_attr = {
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 },
        .session_id = 42,
        .bridge_epoch = 1000ULL,
        .seq = 17,
        .node_id = 0,
        .timestamp_utc_ms = 1792380000000ULL,
        .time_quality = TIME_QUALITY_SYNCED
    };
    len = synth_format_mac_attributes(buffer, sizeof(buffer), &tuple_attr, true, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"macAddress\":\"E4:5F:01:23:45:67\"") != NULL);
    assert(strstr(buffer, "\"bleConnected\":true") != NULL);
    assert(strstr(buffer, "\"gatewayId\":\"gw-01\"") != NULL);
    assert(strstr(buffer, "\"bridgeEpoch\":\"00000000000003E8\"") != NULL);
    assert(strstr(buffer, "\"sessionId\":42") != NULL);
    assert(strstr(buffer, "\"statusSeq\":17") != NULL);
    assert(strstr(buffer, "\"statusTs\":1792380000000") != NULL);
    printf("      -> PASS: CLIENT_SCOPE attributes serialized with full metadata: %s\n", buffer);

    /* ─── TEST 9: MAC-Native Vitals Telemetry ─── */
    printf("[RUN] Test 9: Verify MAC-native vitals telemetry...\n");
    frame_snapshot_tuple_t tuple_vitals = {
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 },
        .session_id = 42,
        .bridge_epoch = 1000ULL,
        .seq = 18,
        .node_id = 0,
        .timestamp_utc_ms = 1792380000123ULL,
        .time_quality = TIME_QUALITY_SYNCED
    };
    vitals_packet_t v_mac = {
        .hr_ecg = 72,
        .hr_ppg = 70,
        .spo2 = 98,
        .temp_x10 = 365
    };
    len = synth_format_mac_vitals(buffer, sizeof(buffer), &tuple_vitals, &v_mac, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"E4:5F:01:23:45:67\"") != NULL);
    assert(strstr(buffer, "\"ts\":1792380000123") != NULL);
    assert(strstr(buffer, "\"spo2\":98") != NULL);
    assert(strstr(buffer, "\"ecgHeartRate\":72") != NULL);
    assert(strstr(buffer, "\"ppgHeartRate\":70") != NULL);
    assert(strstr(buffer, "\"temperature\":36.5") != NULL);
    assert(strstr(buffer, "\"gatewayId\":\"gw-01\"") != NULL);
    assert(strstr(buffer, "\"bridgeEpoch\":\"00000000000003E8\"") != NULL);
    assert(strstr(buffer, "\"sessionId\":42") != NULL);
    assert(strstr(buffer, "\"seq\":18") != NULL);
    assert(strstr(buffer, "\"timeQuality\":\"SYNCED\"") != NULL);
    assert(strstr(buffer, "\"timeBasis\":\"CENTRAL_RX\"") != NULL);
    printf("      -> PASS: Vitals telemetry serialized with full metadata: %s\n", buffer);

    /* ─── TEST 9b: MAC-Native Vitals with Battery ─── */
    printf("[RUN] Test 9b: Verify MAC vitals with battery telemetry...\n");
    vitals_packet_t v_mac_bat = v_mac;
    v_mac_bat.battery = 92;
    len = synth_format_mac_vitals(buffer, sizeof(buffer), &tuple_vitals, &v_mac_bat, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"battery\":92") != NULL);
    printf("      -> PASS: MAC vitals telemetry serialized with battery percentage\n");

    /* ─── TEST 10: Unsynchronized Time Rejection (No Fallback) ─── */
    printf("[RUN] Test 10: Verify unsynchronized time rejects serialization (returns -2)...\n");
    frame_snapshot_tuple_t tuple_unsynced = tuple_vitals;
    tuple_unsynced.time_quality = TIME_QUALITY_UNKNOWN;
    tuple_unsynced.timestamp_utc_ms = 0;
    assert(synth_format_mac_vitals(buffer, sizeof(buffer), &tuple_unsynced, &v_mac, "gw-01") == -2);
    assert(synth_format_mac_ecg_batch(buffer, sizeof(buffer), &tuple_unsynced, ecg_samples, 50, "gw-01") == -2);
    printf("      -> PASS: Unsynchronized frames safely rejected without fake server timestamps\n");

    /* ─── TEST 11: MAC-Native ECG Batch Telemetry ─── */
    printf("[RUN] Test 11: Verify MAC-native ECG batch telemetry...\n");
    frame_snapshot_tuple_t tuple_ecg = {
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 },
        .session_id = 42,
        .bridge_epoch = 1000ULL,
        .seq = 19,
        .node_id = 0,
        .timestamp_utc_ms = 1792380000123ULL,
        .time_quality = TIME_QUALITY_SYNCED
    };
    int16_t ecg_short[3] = { 500, 510, 520 };
    len = synth_format_mac_ecg_batch(buffer, sizeof(buffer), &tuple_ecg, ecg_short, 3, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"ecg_batch\":\"[500,510,520]\"") != NULL);
    assert(strstr(buffer, "\"samplePeriodMs\":4") != NULL);
    assert(strstr(buffer, "\"sampleCount\":3") != NULL);
    assert(strstr(buffer, "\"batchEndTs\":1792380000123") != NULL);
    assert(strstr(buffer, "\"timeQuality\":\"SYNCED\"") != NULL);
    assert(strstr(buffer, "\"timeBasis\":\"CENTRAL_RX\"") != NULL);
    printf("      -> PASS: ECG batch telemetry serialized with full metadata: %s\n", buffer);

    /* ─── TEST 12: MAC-Native Disconnect CLIENT_SCOPE attributes (bleConnected=false) ─── */
    printf("[RUN] Test 12: Verify MAC-native disconnect attributes (bleConnected=false)...\n");
    len = synth_format_mac_attributes(buffer, sizeof(buffer), &tuple_attr, false, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"bleConnected\":false") != NULL);
    assert(strstr(buffer, "\"macAddress\":\"E4:5F:01:23:45:67\"") != NULL);
    printf("      -> PASS: Disconnect attributes serialized with bleConnected=false: %s\n", buffer);

    /* ─── TEST 13: Fast Integer Writer Edge Values (INT16_MIN, INT16_MAX, 0, Negative) ─── */
    printf("[RUN] Test 13: Verify fast integer writer with boundary values (INT16_MIN, INT16_MAX, 0, negative)...\n");
    int16_t edge_samples[6] = { 0, -1, 1, -32768, 32767, -999 };
    len = synth_format_mac_ecg_batch(buffer, sizeof(buffer), &tuple_ecg, edge_samples, 6, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"ecg_batch\":\"[0,-1,1,-32768,32767,-999]\"") != NULL);

    len = synth_format_ecg_batch(buffer, sizeof(buffer), 0, edge_samples, 6);
    assert(len > 0);
    assert(strstr(buffer, "\"ecg_batch\":\"[0,-1,1,-32768,32767,-999]\"") != NULL);
    printf("      -> PASS: INT16_MIN (-32768) and INT16_MAX (32767) serialized byte-for-byte correctly\n");

    /* ─── TEST 14: Single-Sample Batch (count = 1) ─── */
    printf("[RUN] Test 14: Verify single-sample batch (count = 1, no trailing comma)...\n");
    int16_t single_sample[1] = { -42 };
    len = synth_format_mac_ecg_batch(buffer, sizeof(buffer), &tuple_ecg, single_sample, 1, "gw-01");
    assert(len > 0);
    assert(strstr(buffer, "\"ecg_batch\":\"[-42]\"") != NULL);
    printf("      -> PASS: Single-sample batch formatted cleanly: %s\n", buffer);

    /* ─── TEST 15: Exact Buffer Boundary & Canary Protection ─── */
    printf("[RUN] Test 15: Verify exact buffer boundary limits and canary bytes...\n");
    /* Measure exact length needed for 50 samples (> 384 bytes minimum guard) */
    int exact_len = synth_format_mac_ecg_batch(buffer, sizeof(buffer), &tuple_ecg, ecg_samples, 50, "gw-01");
    assert(exact_len > 384);

    /* Allocate buffer with canary bytes before and after */
    char boundary_buf[1024];
    memset(boundary_buf, 0xAA, sizeof(boundary_buf));
    char *test_target = boundary_buf + 16;

    /* Calling with exact length + 1 (room for null terminator) must succeed */
    int fit_len = synth_format_mac_ecg_batch(test_target, exact_len + 1, &tuple_ecg, ecg_samples, 50, "gw-01");
    assert(fit_len == exact_len);
    /* Verify canaries untouched */
    for (int i = 0; i < 16; i++) {
        assert((uint8_t)boundary_buf[i] == 0xAA);
        assert((uint8_t)test_target[exact_len + 1 + i] == 0xAA);
    }

    /* Calling with exact length (no room for null terminator) must return -1 without corrupting canaries */
    memset(boundary_buf, 0x55, sizeof(boundary_buf));
    int trunc_len = synth_format_mac_ecg_batch(test_target, exact_len, &tuple_ecg, ecg_samples, 50, "gw-01");
    assert(trunc_len == -1);
    for (int i = 0; i < 16; i++) {
        assert((uint8_t)boundary_buf[i] == 0x55);
        assert((uint8_t)test_target[exact_len + i] == 0x55);
    }
    printf("      -> PASS: Buffer boundary limits enforced; zero memory overrun detected\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL DATA SYNTHESIZER UNIT TESTS PASSED (100%%)! \n");
    printf("====================================================\n");
    return 0;
}
