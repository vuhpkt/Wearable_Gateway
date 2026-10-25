/**
 * @file downlink_synthesizer.c
 * @brief Zero-Heap Downlink JSON Scanner & Command Synthesizer for ESP32 Cloud Gateway
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_protocol_def.h"
#include "downlink_synthesizer.h"

/* ══════════════════════════════════════════════════════════════
 *  ASCII MAC PARSER
 * ════════════════════════════════════════════════════════════ */
static inline int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mac_ascii_to_bytes(const char *mac_str, uint8_t mac_out[6]) {
    if (!mac_str || !mac_out) {
        return -1;
    }

    /* Trim leading whitespace */
    while (*mac_str && isspace((unsigned char)*mac_str)) {
        mac_str++;
    }
    if (*mac_str == '\0') {
        return -1;
    }

    uint8_t parsed[6] = {0};
    int byte_idx = 0;
    const char *p = mac_str;

    while (*p && byte_idx < 6) {
        int hi = hex_char_to_val(*p++);
        if (hi < 0) return -1;
        if (*p == '\0') return -1;
        int lo = hex_char_to_val(*p++);
        if (lo < 0) return -1;

        parsed[byte_idx++] = (uint8_t)((hi << 4) | lo);

        if (byte_idx < 6) {
            if (*p == ':' || *p == '-') {
                p++;
            } else {
                return -1;
            }
        }
    }

    /* Trim trailing whitespace */
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0' || byte_idx != 6) {
        return -1;
    }

    /* Check for all-zeros */
    if (parsed[0] == 0 && parsed[1] == 0 && parsed[2] == 0 &&
        parsed[3] == 0 && parsed[4] == 0 && parsed[5] == 0) {
        return -2;
    }

    /* Check for broadcast (all FF) */
    if (parsed[0] == 0xFF && parsed[1] == 0xFF && parsed[2] == 0xFF &&
        parsed[3] == 0xFF && parsed[4] == 0xFF && parsed[5] == 0xFF) {
        return -3;
    }

    memcpy(mac_out, parsed, 6);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  LIGHTWEIGHT ZERO-HEAP JSON KEY-VALUE SCANNER
 * ════════════════════════════════════════════════════════════ */
static const char *json_find_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t key_len = strlen(key);
    const char *p = json;

    while ((p = strstr(p, key)) != NULL) {
        /* Verify key is enclosed in quotes: "key" */
        if (p > json && *(p - 1) == '"' && *(p + key_len) == '"') {
            const char *colon = p + key_len + 1;
            while (*colon && isspace((unsigned char)*colon)) colon++;
            if (*colon == ':') {
                colon++;
                while (*colon && isspace((unsigned char)*colon)) colon++;
                return colon;
            }
        }
        p += key_len;
    }
    return NULL;
}

static bool json_read_int(const char *json, const char *key, int32_t *val_out) {
    const char *p = json_find_key(json, key);
    if (!p) return false;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') {
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (strncmp(p, "true", 4) == 0 || strncmp(p, "TRUE", 4) == 0) {
        if (val_out) *val_out = 1;
        return true;
    }
    if (strncmp(p, "false", 5) == 0 || strncmp(p, "FALSE", 5) == 0) {
        if (val_out) *val_out = 0;
        return true;
    }
    char *endptr;
    long v = strtol(p, &endptr, 10);
    if (endptr == p) return false;
    if (val_out) *val_out = (int32_t)v;
    return true;
}

static bool json_read_float(const char *json, const char *key, float *val_out) {
    const char *p = json_find_key(json, key);
    if (!p) return false;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') {
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    char *endptr;
    float v = strtof(p, &endptr);
    if (endptr == p) return false;
    if (val_out) *val_out = v;
    return true;
}

static bool json_read_string(const char *json, const char *key, char *str_out, size_t max_len) {
    const char *p = json_find_key(json, key);
    if (!p || *p != '"') return false;
    p++; // Skip opening quote
    size_t idx = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p++; // Skip escape char
        }
        if (idx < max_len - 1) {
            str_out[idx++] = *p;
        }
        p++;
    }
    str_out[idx] = '\0';
    return (*p == '"');
}

/* ══════════════════════════════════════════════════════════════
 *  COMMAND SYNTHESIS IMPLEMENTATION
 * ════════════════════════════════════════════════════════════ */
int downlink_synth_parse_json(const char *json_str, uint8_t *out_buf, uint8_t max_out_len, uint8_t *out_len) {
    if (!json_str || !out_buf || !out_len) {
        return -1;
    }

    /* 1. Mode Configuration: CMD_MODE_CFG (0xCB) - 7 bytes */
    if (json_find_key(json_str, "deviceMode") != NULL ||
        json_find_key(json_str, "periodicInterval") != NULL ||
        json_find_key(json_str, "captureWindow") != NULL ||
        json_find_key(json_str, "showEcg") != NULL ||
        json_find_key(json_str, "ecgEnabled") != NULL) {
        if (max_out_len < 7) return -2;

        int32_t mode = 0, period = 30, capture = 10, ecg_en = 1;
        json_read_int(json_str, "deviceMode", &mode);
        json_read_int(json_str, "periodicInterval", &period);
        json_read_int(json_str, "captureWindow", &capture);
        if (!json_read_int(json_str, "showEcg", &ecg_en)) {
            json_read_int(json_str, "ecgEnabled", &ecg_en);
        }

        if (mode < 0 || mode >= 3) {
            return -3; // Invalid mode (0=CONTINUOUS, 1=PERIODIC, 2=ECG)
        }

        out_buf[0] = CMD_MODE_CFG;
        out_buf[1] = (uint8_t)mode;
        out_buf[2] = (uint8_t)(period & 0xFF);
        out_buf[3] = (uint8_t)((period >> 8) & 0xFF);
        out_buf[4] = (uint8_t)(capture & 0xFF);
        out_buf[5] = (uint8_t)((capture >> 8) & 0xFF);
        out_buf[6] = (uint8_t)(ecg_en ? 1 : 0);
        *out_len = 7;
        return 0;
    }

    /* 2. Vital Interval Configuration: CMD_VITAL_CFG (0xCC) - 3 or 5 bytes */
    if (json_find_key(json_str, "vitalInterval") != NULL ||
        json_find_key(json_str, "lcdInterval") != NULL) {
        int32_t vital_int = 1000, lcd_int = 0;
        bool has_vital = json_read_int(json_str, "vitalInterval", &vital_int);
        bool has_lcd = json_read_int(json_str, "lcdInterval", &lcd_int);

        if (!has_vital && !has_lcd) return -1;
        if (vital_int <= 0) return -3;

        if (has_lcd && lcd_int > 0) {
            if (max_out_len < 5) return -2;
            out_buf[0] = CMD_VITAL_CFG;
            out_buf[1] = (uint8_t)(vital_int & 0xFF);
            out_buf[2] = (uint8_t)((vital_int >> 8) & 0xFF);
            out_buf[3] = (uint8_t)(lcd_int & 0xFF);
            out_buf[4] = (uint8_t)((lcd_int >> 8) & 0xFF);
            *out_len = 5;
        } else {
            if (max_out_len < 3) return -2;
            out_buf[0] = CMD_VITAL_CFG;
            out_buf[1] = (uint8_t)(vital_int & 0xFF);
            out_buf[2] = (uint8_t)((vital_int >> 8) & 0xFF);
            *out_len = 3;
        }
        return 0;
    }

    /* 3. PPG Configuration: CMD_PPG_CFG (0xCD) - 4 bytes */
    if (json_find_key(json_str, "ppgSampleFreq") != NULL ||
        json_find_key(json_str, "ppgHrSource") != NULL) {
        if (max_out_len < 4) return -2;

        int32_t freq = 100, hr_src = 0;
        json_read_int(json_str, "ppgSampleFreq", &freq);
        json_read_int(json_str, "ppgHrSource", &hr_src);

        if (freq <= 0) return -3;

        out_buf[0] = CMD_PPG_CFG;
        out_buf[1] = (uint8_t)(freq & 0xFF);
        out_buf[2] = (uint8_t)((freq >> 8) & 0xFF);
        out_buf[3] = (uint8_t)(hr_src ? 1 : 0);
        *out_len = 4;
        return 0;
    }

    /* 4. ECG Configuration: CMD_ECG_CFG (0xCF) - 5 bytes (Hardcoded 250 Hz) */
    if (json_find_key(json_str, "ecgSampleFreq") != NULL ||
        json_find_key(json_str, "ecgPacketInterval") != NULL) {
        if (max_out_len < 5) return -2;

        int32_t interval = 200;
        json_read_int(json_str, "ecgPacketInterval", &interval);
        if (interval <= 0) return -3;

        /* Force 250 Hz regardless of what client asked for */
        uint16_t freq_locked = 250;

        out_buf[0] = CMD_ECG_CFG;
        out_buf[1] = (uint8_t)(freq_locked & 0xFF);
        out_buf[2] = (uint8_t)((freq_locked >> 8) & 0xFF);
        out_buf[3] = (uint8_t)(interval & 0xFF);
        out_buf[4] = (uint8_t)((interval >> 8) & 0xFF);
        *out_len = 5;
        return 0;
    }

    /* 5. Threshold Configuration: CMD_THR (0xCE) - 31 bytes */
    if (json_find_key(json_str, "ppgHr_norm_min") != NULL ||
        json_find_key(json_str, "temp_norm_min") != NULL) {
        if (max_out_len < 31) return -2;

        out_buf[0] = CMD_THR;

        /* Defaults */
        int32_t ppg_norm_min = 60, ppg_norm_max = 100;
        int32_t ppg_warn_min = 50, ppg_warn_max = 120;
        int32_t ppg_dang_min = 40, ppg_dang_max = 130;

        int32_t ecg_norm_min = 60, ecg_norm_max = 100;
        int32_t ecg_warn_min = 50, ecg_warn_max = 120;
        int32_t ecg_dang_min = 40, ecg_dang_max = 130;

        int32_t spo2_norm_min = 95, spo2_norm_max = 100;
        int32_t spo2_warn_min = 90, spo2_warn_max = 100;
        int32_t spo2_dang_min = 88, spo2_dang_max = 100;

        float t_norm_min = 36.1f, t_norm_max = 37.2f;
        float t_warn_min = 35.5f, t_warn_max = 38.5f;
        float t_dang_min = 35.0f, t_dang_max = 39.5f;

        json_read_int(json_str, "ppgHr_norm_min", &ppg_norm_min);
        json_read_int(json_str, "ppgHr_norm_max", &ppg_norm_max);
        json_read_int(json_str, "ppgHr_warn_min", &ppg_warn_min);
        json_read_int(json_str, "ppgHr_warn_max", &ppg_warn_max);
        json_read_int(json_str, "ppgHr_dang_min", &ppg_dang_min);
        json_read_int(json_str, "ppgHr_dang_max", &ppg_dang_max);

        json_read_int(json_str, "ecgHr_norm_min", &ecg_norm_min);
        json_read_int(json_str, "ecgHr_norm_max", &ecg_norm_max);
        json_read_int(json_str, "ecgHr_warn_min", &ecg_warn_min);
        json_read_int(json_str, "ecgHr_warn_max", &ecg_warn_max);
        json_read_int(json_str, "ecgHr_dang_min", &ecg_dang_min);
        json_read_int(json_str, "ecgHr_dang_max", &ecg_dang_max);

        json_read_int(json_str, "spo2_norm_min", &spo2_norm_min);
        json_read_int(json_str, "spo2_norm_max", &spo2_norm_max);
        json_read_int(json_str, "spo2_warn_min", &spo2_warn_min);
        json_read_int(json_str, "spo2_warn_max", &spo2_warn_max);
        json_read_int(json_str, "spo2_dang_min", &spo2_dang_min);
        json_read_int(json_str, "spo2_dang_max", &spo2_dang_max);

        json_read_float(json_str, "temp_norm_min", &t_norm_min);
        json_read_float(json_str, "temp_norm_max", &t_norm_max);
        json_read_float(json_str, "temp_warn_min", &t_warn_min);
        json_read_float(json_str, "temp_warn_max", &t_warn_max);
        json_read_float(json_str, "temp_dang_min", &t_dang_min);
        json_read_float(json_str, "temp_dang_max", &t_dang_max);

        /* PPG HR [1-6] */
        out_buf[1] = (uint8_t)ppg_norm_min;
        out_buf[2] = (uint8_t)ppg_norm_max;
        out_buf[3] = (uint8_t)ppg_warn_min;
        out_buf[4] = (uint8_t)ppg_warn_max;
        out_buf[5] = (uint8_t)ppg_dang_min;
        out_buf[6] = (uint8_t)ppg_dang_max;

        /* ECG HR [7-12] */
        out_buf[7] = (uint8_t)ecg_norm_min;
        out_buf[8] = (uint8_t)ecg_norm_max;
        out_buf[9] = (uint8_t)ecg_warn_min;
        out_buf[10] = (uint8_t)ecg_warn_max;
        out_buf[11] = (uint8_t)ecg_dang_min;
        out_buf[12] = (uint8_t)ecg_dang_max;

        /* SpO2 [13-18] */
        out_buf[13] = (uint8_t)spo2_norm_min;
        out_buf[14] = (uint8_t)spo2_norm_max;
        out_buf[15] = (uint8_t)spo2_warn_min;
        out_buf[16] = (uint8_t)spo2_warn_max;
        out_buf[17] = (uint8_t)spo2_dang_min;
        out_buf[18] = (uint8_t)spo2_dang_max;

        /* Temp (uint16 LE, x10) [19-30] */
        uint16_t tn_min = (uint16_t)(t_norm_min * 10.0f + 0.5f);
        uint16_t tn_max = (uint16_t)(t_norm_max * 10.0f + 0.5f);
        uint16_t tw_min = (uint16_t)(t_warn_min * 10.0f + 0.5f);
        uint16_t tw_max = (uint16_t)(t_warn_max * 10.0f + 0.5f);
        uint16_t td_min = (uint16_t)(t_dang_min * 10.0f + 0.5f);
        uint16_t td_max = (uint16_t)(t_dang_max * 10.0f + 0.5f);

        out_buf[19] = (uint8_t)(tn_min & 0xFF);
        out_buf[20] = (uint8_t)((tn_min >> 8) & 0xFF);
        out_buf[21] = (uint8_t)(tn_max & 0xFF);
        out_buf[22] = (uint8_t)((tn_max >> 8) & 0xFF);
        out_buf[23] = (uint8_t)(tw_min & 0xFF);
        out_buf[24] = (uint8_t)((tw_min >> 8) & 0xFF);
        out_buf[25] = (uint8_t)(tw_max & 0xFF);
        out_buf[26] = (uint8_t)((tw_max >> 8) & 0xFF);
        out_buf[27] = (uint8_t)(td_min & 0xFF);
        out_buf[28] = (uint8_t)((td_min >> 8) & 0xFF);
        out_buf[29] = (uint8_t)(td_max & 0xFF);
        out_buf[30] = (uint8_t)((td_max >> 8) & 0xFF);

        *out_len = 31;
        return 0;
    }

    /* 6. Patient Name Configuration: CMD_NAME_CFG (0xC9) - 2..17 bytes */
    if (json_find_key(json_str, "patientName") != NULL) {
        char name[32];
        if (!json_read_string(json_str, "patientName", name, sizeof(name))) {
            return -1;
        }

        uint8_t name_len = (uint8_t)strlen(name);
        if (name_len > 15) {
            name_len = 15; // Safe truncation to 15 chars max
        }

        if (max_out_len < (uint8_t)(2 + name_len)) return -2;

        out_buf[0] = CMD_NAME_CFG;
        out_buf[1] = name_len;
        memcpy(&out_buf[2], name, name_len);
        *out_len = (uint8_t)(2 + name_len);
        return 0;
    }

    /* 7. Time Synchronization: CMD_TIME_CFG (0xC8) - 7 bytes */
    if (json_find_key(json_str, "syncTime") != NULL ||
        json_find_key(json_str, "epochUtc") != NULL ||
        json_find_key(json_str, "time") != NULL) {
        if (max_out_len < 7) return -2;

        int32_t epoch = 0;
        int32_t tz = 420; // Default +420 min (UTC+7)
        if (!json_read_int(json_str, "epochUtc", &epoch)) {
            if (!json_read_int(json_str, "time", &epoch)) {
                epoch = (int32_t)time(NULL);
            }
        }
        json_read_int(json_str, "tzOffsetMin", &tz);

        out_buf[0] = CMD_TIME_CFG;
        out_buf[1] = (uint8_t)(epoch & 0xFF);
        out_buf[2] = (uint8_t)((epoch >> 8) & 0xFF);
        out_buf[3] = (uint8_t)((epoch >> 16) & 0xFF);
        out_buf[4] = (uint8_t)((epoch >> 24) & 0xFF);
        out_buf[5] = (uint8_t)(tz & 0xFF);
        out_buf[6] = (uint8_t)((tz >> 8) & 0xFF);
        *out_len = 7;
        return 0;
    }

    return -1; // No matching supported downlink attribute
}

int downlink_synth_extract_device_and_cmd(const char *json_str,
                                          char *device_out, size_t max_device_len,
                                          uint8_t *cmd_out, uint8_t max_cmd_len, uint8_t *cmd_len_out) {
    if (!json_str || !device_out || !cmd_out || !cmd_len_out) {
        return -1;
    }

    /* Extract "device" string */
    if (!json_read_string(json_str, "device", device_out, max_device_len)) {
        return -1;
    }

    /* Locate "data" object */
    const char *data_ptr = json_find_key(json_str, "data");
    if (!data_ptr || *data_ptr != '{') {
        return -2;
    }

    return downlink_synth_parse_json(data_ptr, cmd_out, max_cmd_len, cmd_len_out);
}
