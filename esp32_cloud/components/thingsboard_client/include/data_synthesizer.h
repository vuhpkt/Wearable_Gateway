#pragma once

/* Standard C Libraries */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Project Local Headers */
#include "ble_protocol_def.h"


#define SYNTH_SAMPLE_INTERVAL_MS    4U   /* 4 ms / sample @ 250 Hz */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Đóng gói mảng mẫu ECG thành chuỗi JSON ThingsBoard Gateway API
 * Format chuẩn Wearable_Server: {"Node1":[{"ts":...,"values":{"ecg_batch":"[500,510,...]"}}]}
 *
 * @param dest Buffer chứa chuỗi JSON kết quả
 * @param max_len Dung lượng tối đa của dest
 * @param node_id Chỉ số node (0 .. 9) -> tạo ra "Node1" .. "Node10"
 * @param samples Mảng các mẫu ECG int16_t
 * @param count Số lượng mẫu (thường là 50 mẫu)
 * @return Độ dài chuỗi JSON (bytes), hoặc < 0 nếu lỗi buffer
 */
int synth_format_ecg_batch(char *dest, size_t max_len, uint8_t node_id, 
                           const int16_t *samples, uint16_t count);

/**
 * @brief Đóng gói Vitals (nhịp tim, SpO2, nhiệt độ) thành chuỗi JSON ThingsBoard Gateway API
 *
 * @param dest Buffer chứa chuỗi JSON kết quả
 * @param max_len Dung lượng tối đa của dest
 * @param node_id Chỉ số node (0 .. 9) -> tạo ra "Node1" .. "Node10"
 * @param v Con trỏ tới struct vitals_packet_t
 * @return Độ dài chuỗi JSON (bytes), 0 nếu không có giá trị > 0, hoặc < 0 nếu lỗi buffer
 */
int synth_format_vitals(char *dest, size_t max_len, uint8_t node_id, 
                        const vitals_packet_t *v);

/**
 * @brief Đóng gói sự kiện kết nối/ngắt kết nối Node thành chuỗi JSON Gateway API
 * Connect: topic "v1/gateway/connect", payload {"device":"Node1","type":"default"}
 * Disconnect: topic "v1/gateway/disconnect", payload {"device":"Node1"}
 *
 * @param dest Buffer chứa chuỗi JSON
 * @param max_len Dung lượng tối đa của dest
 * @param node_id Chỉ số node (0 .. 9)
 * @param is_connected true nếu kết nối, false nếu ngắt kết nối
 * @param out_topic Con trỏ nhận chuỗi topic cần publish
 * @return Độ dài chuỗi JSON (bytes), hoặc < 0 nếu lỗi
 */
int synth_format_node_status(char *dest, size_t max_len, uint8_t node_id, 
                             bool is_connected, const char **out_topic);

#ifdef __cplusplus
}
#endif
