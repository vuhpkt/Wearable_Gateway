/* Standard C & POSIX Libraries */
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

/* Project Local Headers */
#include "data_synthesizer.h"

static uint64_t get_epoch_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1700000000) {
        return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
    }
    return 0; /* Trả về 0 nếu chưa đồng bộ SNTP để tránh gửi timestamp năm 1970 */
}

int synth_format_ecg_batch(char *dest, size_t max_len, uint8_t node_id, 
                           const int16_t *samples, uint16_t count) {
    if (!dest || !samples || count == 0 || max_len < 128) return -1;

    uint64_t ts = get_epoch_ms();
    uint8_t node_num = node_id + 1; /* Quy ước Wearable_Server: Node1 .. Node10 */

    int len = 0;
    if (ts > 0) {
        len = snprintf(dest, max_len, 
                       "{\"Node%u\":[{\"ts\":%llu,\"values\":{\"ecg_batch\":\"[", 
                       node_num, (unsigned long long)ts);
    } else {
        len = snprintf(dest, max_len, 
                       "{\"Node%u\":[{\"values\":{\"ecg_batch\":\"[", 
                       node_num);
    }
    if (len < 0 || (size_t)len >= max_len) return -1;

    for (uint16_t i = 0; i < count; i++) {
        int w = snprintf(dest + len, max_len - len, "%s%d", (i > 0) ? "," : "", samples[i]);
        if (w < 0 || (size_t)(len + w) >= max_len) return -1;
        len += w;
    }

    int tail = snprintf(dest + len, max_len - len, "]\"}}]}");
    if (tail < 0 || (size_t)(len + tail) >= max_len) return -1;

    return len + tail;
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

    if (v_len == 0) return 0; /* Không có dữ liệu hợp lệ để gửi */

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
