/* Standard C & POSIX Libraries */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* Project Local Headers */
#include "data_synthesizer.h"
#include "bridge_time.h"

static const char *get_quality_str(uint8_t quality) {
    switch (quality) {
    case TIME_QUALITY_SYNCED:    return "SYNCED";
    case TIME_QUALITY_ESTIMATED: return "ESTIMATED";
    case TIME_QUALITY_INVALID:   return "INVALID";
    default:                     return "UNKNOWN";
    }
}

static uint64_t get_epoch_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1700000000) {
        return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
    }
    return 0;
}

/**
 * @brief Fast bounded serialization of an int16_t array as a CSV string without format string parsing.
 * Format: "val0,val1,val2,..."
 * Handles negative numbers, 0, INT16_MIN (-32768), and INT16_MAX (32767).
 *
 * @param dest Destination buffer.
 * @param max_len Total available bytes in buffer starting at dest.
 * @param samples Array of int16_t samples.
 * @param count Number of samples.
 * @return Number of bytes written (excluding null terminator), or -1 on overflow / invalid input.
 */
static int fast_format_ecg_samples(char *dest, size_t max_len, const int16_t *samples, uint16_t count) {
    if (!dest || !samples || max_len == 0) {
        return -1;
    }
    if (count == 0) {
        *dest = '\0';
        return 0;
    }

    char *p = dest;
    const char *end = dest + max_len;

    for (uint16_t i = 0; i < count; i++) {
        /* Add separator comma for i > 0 */
        if (i > 0) {
            if (p >= end) {
                return -1;
            }
            *p++ = ',';
        }

        int16_t val = samples[i];
        if (val == 0) {
            if (p >= end) {
                return -1;
            }
            *p++ = '0';
        } else {
            uint32_t uval;
            if (val < 0) {
                if (p >= end) {
                    return -1;
                }
                *p++ = '-';
                uval = (uint32_t)(-(int32_t)val);
            } else {
                uval = (uint32_t)val;
            }

            /* Extract digits in reverse: max 5 digits for int16 (0..32768) */
            char digits[5];
            uint8_t d_count = 0;
            while (uval > 0) {
                digits[d_count++] = (char)('0' + (uval % 10));
                uval /= 10;
            }

            /* Check remaining capacity for digits */
            if ((size_t)(end - p) < d_count) {
                return -1;
            }

            /* Write digits in correct forward order */
            while (d_count > 0) {
                *p++ = digits[--d_count];
            }
        }
    }

    /* Null terminate if space permits (safe check for string consumers) */
    if (p < end) {
        *p = '\0';
    } else {
        return -1; /* Must have space for at least 1 null terminator */
    }

    return (int)(p - dest);
}

/* ══════════════════════════════════════════════════════════════
 *  MAC-Native Formatter Implementations (Architecture v2)
 * ════════════════════════════════════════════════════════════ */

int synth_format_mac_connect(char *dest, size_t max_len, const uint8_t *mac, const char **out_topic) {
    if (!dest || !mac || max_len < 64) return -1;
    if (out_topic) *out_topic = "v1/gateway/connect";
    int len = snprintf(dest, max_len,
                       "{\"device\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"type\":\"Wearable\"}",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (len < 0 || (size_t)len >= max_len) return -1;
    return len;
}

int synth_format_mac_disconnect(char *dest, size_t max_len, const uint8_t *mac, const char **out_topic) {
    if (!dest || !mac || max_len < 48) return -1;
    if (out_topic) *out_topic = "v1/gateway/disconnect";
    int len = snprintf(dest, max_len,
                       "{\"device\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (len < 0 || (size_t)len >= max_len) return -1;
    return len;
}

int synth_format_mac_attributes(char *dest, size_t max_len,
                               const frame_snapshot_tuple_t *tuple,
                               bool ble_connected,
                               const char *gateway_id) {
    if (!dest || !tuple || max_len < 192) return -1;
    const char *gw = gateway_id ? gateway_id : DEFAULT_GATEWAY_ID;

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             tuple->mac[0], tuple->mac[1], tuple->mac[2],
             tuple->mac[3], tuple->mac[4], tuple->mac[5]);

    int len = snprintf(dest, max_len,
                       "{\"%s\":{\"macAddress\":\"%s\",\"bleConnected\":%s,"
                       "\"gatewayId\":\"%s\",\"bridgeEpoch\":\"%016llX\","
                       "\"sessionId\":%lu,\"statusSeq\":%lu,\"statusTs\":%llu}}",
                       mac_str, mac_str, ble_connected ? "true" : "false",
                       gw, (unsigned long long)tuple->bridge_epoch,
                       (unsigned long)tuple->session_id, (unsigned long)tuple->seq,
                       (unsigned long long)tuple->timestamp_utc_ms);
    if (len < 0 || (size_t)len >= max_len) return -1;
    return len;
}

int synth_format_mac_vitals(char *dest, size_t max_len,
                           const frame_snapshot_tuple_t *tuple,
                           const vitals_packet_t *v,
                           const char *gateway_id) {
    if (!dest || !tuple || !v || max_len < 256) return -1;

    /* Timestamp check: do not fallback to server time when unsynchronized */
    if (tuple->timestamp_utc_ms == 0 || tuple->time_quality == TIME_QUALITY_UNKNOWN) {
        return -2;
    }

    const char *gw = gateway_id ? gateway_id : DEFAULT_GATEWAY_ID;
    float temp_c = (float)v->temp_x10 / 10.0f;

    char values[128] = {0};
    int v_len = 0;
    if (v->spo2 > 0)     v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"spo2\":%u", (v_len > 0) ? "," : "", v->spo2);
    if (v->hr_ecg > 0)   v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"ecgHeartRate\":%u", (v_len > 0) ? "," : "", v->hr_ecg);
    if (v->hr_ppg > 0)   v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"ppgHeartRate\":%u", (v_len > 0) ? "," : "", v->hr_ppg);
    if (temp_c > 0.0f)   v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"temperature\":%.1f", (v_len > 0) ? "," : "", temp_c);
    if (v->battery > 0 && v->battery <= 100) v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"battery\":%u", (v_len > 0) ? "," : "", v->battery);

    if (v_len == 0) {
        return 0; /* No valid vitals to publish */
    }

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             tuple->mac[0], tuple->mac[1], tuple->mac[2],
             tuple->mac[3], tuple->mac[4], tuple->mac[5]);

    int len = snprintf(dest, max_len,
                       "{\"%s\":[{\"ts\":%llu,\"values\":{%s,"
                       "\"gatewayId\":\"%s\",\"bridgeEpoch\":\"%016llX\","
                       "\"sessionId\":%lu,\"seq\":%lu,"
                       "\"timeQuality\":\"%s\",\"timeBasis\":\"CENTRAL_RX\"}}]}",
                       mac_str, (unsigned long long)tuple->timestamp_utc_ms, values,
                       gw, (unsigned long long)tuple->bridge_epoch,
                       (unsigned long)tuple->session_id, (unsigned long)tuple->seq,
                       get_quality_str(tuple->time_quality));
    if (len < 0 || (size_t)len >= max_len) return -1;
    return len;
}

int synth_format_mac_ecg_batch(char *dest, size_t max_len,
                              const frame_snapshot_tuple_t *tuple,
                              const int16_t *samples,
                              uint16_t count,
                              const char *gateway_id) {
    if (!dest || !tuple || !samples || count == 0 || max_len < 384) return -1;

    if (tuple->timestamp_utc_ms == 0 || tuple->time_quality == TIME_QUALITY_UNKNOWN) {
        return -2;
    }

    const char *gw = gateway_id ? gateway_id : DEFAULT_GATEWAY_ID;

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             tuple->mac[0], tuple->mac[1], tuple->mac[2],
             tuple->mac[3], tuple->mac[4], tuple->mac[5]);

    int head_len = snprintf(dest, max_len,
                            "{\"%s\":[{\"ts\":%llu,\"values\":{\"ecg_batch\":\"[",
                            mac_str, (unsigned long long)tuple->timestamp_utc_ms);
    if (head_len < 0 || (size_t)head_len >= max_len) return -1;

    int samples_len = fast_format_ecg_samples(dest + head_len, max_len - head_len, samples, count);
    if (samples_len < 0) return -1;

    int total_len = head_len + samples_len;
    if ((size_t)total_len >= max_len) return -1;

    int tail = snprintf(dest + total_len, max_len - total_len,
                        "]\",\"samplePeriodMs\":%u,\"sampleCount\":%u,\"batchEndTs\":%llu,"
                        "\"gatewayId\":\"%s\",\"bridgeEpoch\":\"%016llX\","
                        "\"sessionId\":%lu,\"seq\":%lu,"
                        "\"timeQuality\":\"%s\",\"timeBasis\":\"CENTRAL_RX\"}}]}",
                        ECG_SAMPLE_PERIOD_MS, count, (unsigned long long)tuple->timestamp_utc_ms,
                        gw, (unsigned long long)tuple->bridge_epoch,
                        (unsigned long)tuple->session_id, (unsigned long)tuple->seq,
                        get_quality_str(tuple->time_quality));
    if (tail < 0 || (size_t)(total_len + tail) >= max_len) return -1;
    return total_len + tail;
}

/* ══════════════════════════════════════════════════════════════
 *  Legacy Node-ID Formatter Implementations
 * ════════════════════════════════════════════════════════════ */

int synth_format_ecg_batch(char *dest, size_t max_len, uint8_t node_id, 
                           const int16_t *samples, uint16_t count) {
    if (!dest || !samples || count == 0 || max_len < 128) return -1;

    uint64_t ts = get_epoch_ms();
    uint8_t node_num = node_id + 1;

    int head_len = 0;
    if (ts > 0) {
        head_len = snprintf(dest, max_len, 
                            "{\"Node%u\":[{\"ts\":%llu,\"values\":{\"ecg_batch\":\"[", 
                            node_num, (unsigned long long)ts);
    } else {
        head_len = snprintf(dest, max_len, 
                            "{\"Node%u\":[{\"values\":{\"ecg_batch\":\"[", 
                            node_num);
    }
    if (head_len < 0 || (size_t)head_len >= max_len) return -1;

    int samples_len = fast_format_ecg_samples(dest + head_len, max_len - head_len, samples, count);
    if (samples_len < 0) return -1;

    int total_len = head_len + samples_len;
    if ((size_t)total_len >= max_len) return -1;

    int tail = snprintf(dest + total_len, max_len - total_len, "]\"}}]}");
    if (tail < 0 || (size_t)(total_len + tail) >= max_len) return -1;

    return total_len + tail;
}

int synth_format_vitals(char *dest, size_t max_len, uint8_t node_id, 
                        const vitals_packet_t *v) {
    if (!dest || !v || max_len < 128) return -1;

    uint64_t ts = get_epoch_ms();
    uint8_t node_num = node_id + 1;
    float temp_c = (float)v->temp_x10 / 10.0f;

    char values[128] = {0};
    int v_len = 0;
    if (v->hr_ecg > 0) v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"ecgHeartRate\":%u", (v_len > 0) ? "," : "", v->hr_ecg);
    if (v->hr_ppg > 0) v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"ppgHeartRate\":%u", (v_len > 0) ? "," : "", v->hr_ppg);
    if (v->spo2 > 0)   v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"spo2\":%u",         (v_len > 0) ? "," : "", v->spo2);
    if (temp_c > 0.0f) v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"temperature\":%.1f", (v_len > 0) ? "," : "", temp_c);
    if (v->battery > 0 && v->battery <= 100) v_len += snprintf(values + v_len, sizeof(values) - v_len, "%s\"battery\":%u", (v_len > 0) ? "," : "", v->battery);

    if (v_len == 0) return 0;

    if (ts > 0) {
        return snprintf(dest, max_len, 
                        "{\"Node%u\":[{\"ts\":%llu,\"values\":{%s}}]}", 
                        node_num, (unsigned long long)ts, values);
    } else {
        return snprintf(dest, max_len, 
                        "{\"Node%u\":[{\"values\":{%s}}]}", 
                        node_num, values);
    }
}

int synth_format_node_status(char *dest, size_t max_len, uint8_t node_id, 
                             bool is_connected, const char **out_topic) {
    if (!dest || max_len < 64) return -1;

    uint8_t node_num = node_id + 1;
    if (is_connected) {
        if (out_topic) *out_topic = "v1/gateway/connect";
        return snprintf(dest, max_len, "{\"device\":\"Node%u\",\"type\":\"default\"}", node_num);
    } else {
        if (out_topic) *out_topic = "v1/gateway/disconnect";
        return snprintf(dest, max_len, "{\"device\":\"Node%u\"}", node_num);
    }
}
