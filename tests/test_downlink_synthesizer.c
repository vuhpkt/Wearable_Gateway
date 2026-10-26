/**
 * @file test_downlink_synthesizer.c
 * @brief Host Unit Tests for ESP32 Zero-Heap Downlink Synthesizer
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1
#include "ble_protocol_def.h"
#include "downlink_synthesizer.h"

/* ══════════════════════════════════════════════════════════════
 *  SUITE 1: ASCII MAC PARSER TEST SUITE
 * ════════════════════════════════════════════════════════════ */
static void test_mac_ascii_parsing(void) {
    printf("[RUN] Test 1: MAC ASCII Parser validations...\n");
    uint8_t mac[6];

    /* 1.1 Chuẩn hoa và thường */
    assert(mac_ascii_to_bytes("E4:5F:01:23:45:67", mac) == 0);
    assert(mac[0] == 0xE4 && mac[1] == 0x5F && mac[2] == 0x01 && mac[3] == 0x23 && mac[4] == 0x45 && mac[5] == 0x67);

    assert(mac_ascii_to_bytes("e4:5f:01:23:45:67", mac) == 0);
    assert(mac[0] == 0xE4 && mac[1] == 0x5F && mac[2] == 0x01 && mac[3] == 0x23 && mac[4] == 0x45 && mac[5] == 0x67);

    /* 1.2 Dấu phân cách gạch ngang */
    assert(mac_ascii_to_bytes("E4-5F-01-23-45-67", mac) == 0);
    assert(mac[0] == 0xE4 && mac[5] == 0x67);

    /* 1.3 Khoảng trắng đầu cuối */
    assert(mac_ascii_to_bytes("  E4:5F:01:23:45:67  ", mac) == 0);
    assert(mac[0] == 0xE4 && mac[5] == 0x67);

    /* 1.4 Các trường hợp lỗi: ký tự lạ, thiếu ký tự, NULL, rỗng, broadcast, all-zero */
    assert(mac_ascii_to_bytes("E4:5F:GG:23:45:67", mac) != 0); // 'G' không hợp lệ
    assert(mac_ascii_to_bytes("E4:5F:01:23:45", mac) != 0);    // Thiếu byte
    assert(mac_ascii_to_bytes("E4:5F:01:23:45:67:89", mac) != 0); // Quá dài
    assert(mac_ascii_to_bytes("", mac) != 0);
    assert(mac_ascii_to_bytes(NULL, mac) != 0);
    assert(mac_ascii_to_bytes("00:00:00:00:00:00", mac) != 0); // Reject zero MAC
    assert(mac_ascii_to_bytes("FF:FF:FF:FF:FF:FF", mac) != 0); // Reject broadcast MAC
    printf("      -> PASS: MAC ASCII Parser handles valid/invalid cases robustly\n");
}

/* ══════════════════════════════════════════════════════════════
 *  SUITE 2: ZERO-HEAP JSON COMMAND SYNTHESIZER
 * ════════════════════════════════════════════════════════════ */
static void test_cmd_mode_cfg_synthesis(void) {
    printf("[RUN] Test 2: CMD_MODE_CFG (0xCB) Synthesis...\n");
    uint8_t out[32];
    uint8_t len = 0;

    /* Mode 1 (Periodic), Period 30s, Capture 10s, ECG On (1) */
    const char *json_valid = "{\"deviceMode\":1,\"periodicInterval\":30,\"captureWindow\":10,\"ecgEnabled\":1}";
    assert(downlink_synth_parse_json(json_valid, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_MODE_CFG); // 0xCB
    assert(out[1] == 1);            // mode
    assert(out[2] == 30 && out[3] == 0); // period = 30 (LE)
    assert(out[4] == 10 && out[5] == 0); // capture = 10 (LE)
    assert(out[6] == 1);            // ecgEnabled

    /* Out of bounds mode: mode >= 3 phải bị từ chối */
    const char *json_invalid_mode = "{\"deviceMode\":3,\"periodicInterval\":30,\"captureWindow\":10}";
    assert(downlink_synth_parse_json(json_invalid_mode, out, sizeof(out), &len) != 0);

    /* 2.1 Test standalone showEcg: 1 (ECG ON) */
    const char *json_show_ecg_on = "{\"showEcg\":1}";
    assert(downlink_synth_parse_json(json_show_ecg_on, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_MODE_CFG);
    assert(out[1] == 0); // default continuous
    assert(out[6] == 1); // ecgEnabled == 1

    /* 2.2 Test standalone showEcg: 0 (ECG OFF) */
    const char *json_show_ecg_off = "{\"showEcg\":0}";
    assert(downlink_synth_parse_json(json_show_ecg_off, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_MODE_CFG);
    assert(out[6] == 0); // ecgEnabled == 0

    /* 2.3 Test full tuple: deviceMode + periodicInterval + captureWindow + showEcg */
    const char *json_tuple = "{\"deviceMode\":1,\"periodicInterval\":60,\"captureWindow\":15,\"showEcg\":0}";
    assert(downlink_synth_parse_json(json_tuple, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_MODE_CFG);
    assert(out[1] == 1);                 // mode 1
    assert(out[2] == 60 && out[3] == 0); // period = 60
    assert(out[4] == 15 && out[5] == 0); // capture = 15
    assert(out[6] == 0);                 // ecgEnabled = 0

    /* 2.4 Test JSON boolean literals (true / false) */
    const char *json_bool_false = "{\"showEcg\":false}";
    assert(downlink_synth_parse_json(json_bool_false, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_MODE_CFG);
    assert(out[6] == 0); // false parsed as 0

    const char *json_bool_true = "{\"showEcg\":true}";
    assert(downlink_synth_parse_json(json_bool_true, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_MODE_CFG);
    assert(out[6] == 1); // true parsed as 1

    const char *json_bool_ecg_en = "{\"ecgEnabled\":false}";
    assert(downlink_synth_parse_json(json_bool_ecg_en, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[6] == 0); // ecgEnabled false parsed as 0

    /* 2.5 Test string-quoted booleans and integers */
    const char *json_quoted_false = "{\"showEcg\":\"false\"}";
    assert(downlink_synth_parse_json(json_quoted_false, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[6] == 0);

    const char *json_quoted_true = "{\"showEcg\":\"true\"}";
    assert(downlink_synth_parse_json(json_quoted_true, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[6] == 1);

    const char *json_quoted_spaces = "{\"showEcg\": \"  false  \"}";
    assert(downlink_synth_parse_json(json_quoted_spaces, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[6] == 0);

    const char *json_quoted_num = "{\"deviceMode\":\"1\",\"periodicInterval\":\"45\",\"captureWindow\":\"12\",\"showEcg\":\"0\"}";
    assert(downlink_synth_parse_json(json_quoted_num, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[1] == 1);
    assert(out[2] == 45 && out[3] == 0);
    assert(out[4] == 12 && out[5] == 0);
    assert(out[6] == 0);

    printf("      -> PASS: CMD_MODE_CFG serialized accurately (including booleans & quoted primitives)\n");
}

static void test_cmd_ecg_cfg_synthesis(void) {
    printf("[RUN] Test 3: CMD_ECG_CFG (0xCF) Synthesis (Enforcing 250 Hz)...\n");
    uint8_t out[32];
    uint8_t len = 0;

    /* Ngay cả khi client gửi ecgSampleFreq = 500, bộ tổng hợp BẮT BUỘC ép cứng 250 Hz */
    const char *json_ecg = "{\"ecgSampleFreq\":500,\"ecgPacketInterval\":200}";
    assert(downlink_synth_parse_json(json_ecg, out, sizeof(out), &len) == 0);
    assert(len == 5);
    assert(out[0] == CMD_ECG_CFG);  // 0xCF
    uint16_t freq = (uint16_t)out[1] | ((uint16_t)out[2] << 8);
    uint16_t interval = (uint16_t)out[3] | ((uint16_t)out[4] << 8);
    assert(freq == 250);            // HARD-LOCKED AT 250 HZ!
    assert(interval == 200);

    /* Interval bằng 0 phải bị từ chối */
    const char *json_zero_int = "{\"ecgSampleFreq\":250,\"ecgPacketInterval\":0}";
    assert(downlink_synth_parse_json(json_zero_int, out, sizeof(out), &len) != 0);
    printf("      -> PASS: CMD_ECG_CFG locked to 250 Hz SAADC PPI rate\n");
}

static void test_cmd_thr_synthesis(void) {
    printf("[RUN] Test 4: CMD_THR (0xCE) Synthesis (31 bytes with float temp*10)...\n");
    uint8_t out[64];
    uint8_t len = 0;

    const char *json_thr = "{"
        "\"ppgHr_norm_min\":60,\"ppgHr_norm_max\":100,"
        "\"ppgHr_warn_min\":50,\"ppgHr_warn_max\":120,"
        "\"ppgHr_dang_min\":40,\"ppgHr_dang_max\":130,"
        "\"ecgHr_norm_min\":60,\"ecgHr_norm_max\":100,"
        "\"ecgHr_warn_min\":50,\"ecgHr_warn_max\":120,"
        "\"ecgHr_dang_min\":40,\"ecgHr_dang_max\":130,"
        "\"spo2_norm_min\":95,\"spo2_norm_max\":100,"
        "\"spo2_warn_min\":90,\"spo2_warn_max\":100,"
        "\"spo2_dang_min\":88,\"spo2_dang_max\":100,"
        "\"temp_norm_min\":36.1,\"temp_norm_max\":37.2,"
        "\"temp_warn_min\":35.5,\"temp_warn_max\":38.5,"
        "\"temp_dang_min\":35.0,\"temp_dang_max\":39.5"
    "}";

    assert(downlink_synth_parse_json(json_thr, out, sizeof(out), &len) == 0);
    assert(len == 31);
    assert(out[0] == CMD_THR); // 0xCE

    /* Kiểm tra nhiệt độ được scale đúng uint16 LE (36.1 -> 361 = 0x0169) */
    uint16_t t_norm_min = (uint16_t)out[19] | ((uint16_t)out[20] << 8);
    uint16_t t_norm_max = (uint16_t)out[21] | ((uint16_t)out[22] << 8);
    assert(t_norm_min == 361);
    assert(t_norm_max == 372);

    /* Test quoted floats and spaces inside quotes for temperature thresholds */
    const char *json_thr_quoted = "{\"temp_norm_min\": \" 36.5 \", \"temp_norm_max\": \"37.5\"}";
    assert(downlink_synth_parse_json(json_thr_quoted, out, sizeof(out), &len) == 0);
    assert(len == 31);
    uint16_t q_min = (uint16_t)out[19] | ((uint16_t)out[20] << 8);
    uint16_t q_max = (uint16_t)out[21] | ((uint16_t)out[22] << 8);
    assert(q_min == 365);
    assert(q_max == 375);

    printf("      -> PASS: CMD_THR 31-byte payload serialized with precise float scaling (including quoted floats)\n");
}

static void test_cmd_name_cfg_synthesis(void) {
    printf("[RUN] Test 5: CMD_NAME_CFG (0xC9) Synthesis (String safety & truncation)...\n");
    uint8_t out[32];
    uint8_t len = 0;

    /* Tên bình thường 8 ký tự */
    const char *json_name = "{\"patientName\":\"John Doe\"}";
    assert(downlink_synth_parse_json(json_name, out, sizeof(out), &len) == 0);
    assert(len == 2 + 8);
    assert(out[0] == CMD_NAME_CFG);
    assert(out[1] == 8);
    assert(memcmp(&out[2], "John Doe", 8) == 0);

    /* Tên dài 25 ký tự -> Bắt buộc truncate an toàn về 15 ký tự */
    const char *json_long = "{\"patientName\":\"Alexander The Great Super Patient\"}";
    assert(downlink_synth_parse_json(json_long, out, sizeof(out), &len) == 0);
    assert(len == 2 + 15);
    assert(out[1] == 15);
    assert(memcmp(&out[2], "Alexander The G", 15) == 0);
    printf("      -> PASS: CMD_NAME_CFG correctly truncated to 15 chars max\n");
}

static void test_json_edge_cases(void) {
    printf("[RUN] Test 6: Zero-Heap Scanner edge cases...\n");
    uint8_t out[32];
    uint8_t len = 0;

    /* JSON rỗng, JSON không có key liên quan */
    assert(downlink_synth_parse_json("{}", out, sizeof(out), &len) != 0);
    assert(downlink_synth_parse_json("{\"unrelatedKey\":123}", out, sizeof(out), &len) != 0);

    /* Whitespace phong phú, xuống dòng */
    const char *json_spaces = "  \n\t {  \"vitalInterval\"  :   2000  }  \n";
    assert(downlink_synth_parse_json(json_spaces, out, sizeof(out), &len) == 0);
    assert(len == 3);
    assert(out[0] == CMD_VITAL_CFG);
    uint16_t interval = (uint16_t)out[1] | ((uint16_t)out[2] << 8);
    assert(interval == 2000);

    /* Tràn buffer output */
    assert(downlink_synth_parse_json(json_spaces, out, 2, &len) != 0); // out buffer quá nhỏ
    printf("      -> PASS: Scanner rejects malformed inputs and prevents buffer overrun\n");
}

static void test_extract_device_and_cmd(void) {
    printf("[RUN] Test 7: Extract device wrapper and command...\n");
    const char *tb_wrapper = "{\"device\":\"E4:5F:01:23:45:67\",\"data\":{\"vitalInterval\":1500}}";
    char device[32];
    uint8_t cmd[32];
    uint8_t cmd_len = 0;

    assert(downlink_synth_extract_device_and_cmd(tb_wrapper, device, sizeof(device), cmd, sizeof(cmd), &cmd_len) == 0);
    assert(strcmp(device, "E4:5F:01:23:45:67") == 0);
    assert(cmd_len == 3);
    assert(cmd[0] == CMD_VITAL_CFG);
    uint16_t interval = (uint16_t)cmd[1] | ((uint16_t)cmd[2] << 8);
    assert(interval == 1500);

    /* 7.2 Wrapper containing showEcg */
    const char *tb_ecg_wrapper = "{\"device\":\"E4:5F:01:23:45:67\",\"data\":{\"showEcg\":0}}";
    assert(downlink_synth_extract_device_and_cmd(tb_ecg_wrapper, device, sizeof(device), cmd, sizeof(cmd), &cmd_len) == 0);
    assert(strcmp(device, "E4:5F:01:23:45:67") == 0);
    assert(cmd_len == 7);
    assert(cmd[0] == CMD_MODE_CFG);
    assert(cmd[6] == 0); // ecgEnabled == 0
    printf("      -> PASS: Device wrapper extraction verified (vitalInterval & showEcg)\n");
}

static void test_cmd_time_cfg_synthesis(void) {
    printf("[RUN] Test 8: CMD_TIME_CFG (0xC8) Synthesis (Time sync)...\n");
    uint8_t out[32];
    uint8_t len = 0;

    /* Explicit epoch and tzOffsetMin */
    const char *json_time = "{\"epochUtc\":1710000000,\"tzOffsetMin\":420}";
    assert(downlink_synth_parse_json(json_time, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_TIME_CFG);
    uint32_t epoch = (uint32_t)out[1] | ((uint32_t)out[2] << 8) | ((uint32_t)out[3] << 16) | ((uint32_t)out[4] << 24);
    int16_t tz = (int16_t)((uint16_t)out[5] | ((uint16_t)out[6] << 8));
    assert(epoch == 1710000000);
    assert(tz == 420);

    /* syncTime trigger */
    const char *json_sync = "{\"syncTime\":true}";
    assert(downlink_synth_parse_json(json_sync, out, sizeof(out), &len) == 0);
    assert(len == 7);
    assert(out[0] == CMD_TIME_CFG);
    printf("      -> PASS: CMD_TIME_CFG (0xC8) synthesized correctly (epoch and syncTime)\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING DOWNLINK SYNTHESIZER UNIT TESTS (TDD)    \n");
    printf("====================================================\n\n");
    test_mac_ascii_parsing();
    test_cmd_mode_cfg_synthesis();
    test_cmd_ecg_cfg_synthesis();
    test_cmd_thr_synthesis();
    test_cmd_name_cfg_synthesis();
    test_json_edge_cases();
    test_extract_device_and_cmd();
    test_cmd_time_cfg_synthesis();
    printf("\n🎉 ALL DOWNLINK SYNTHESIZER UNIT TESTS PASSED!\n");
    return 0;
}
