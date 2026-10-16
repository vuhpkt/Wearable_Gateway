#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef HOST_TEST
#include "gateway_host_port.h"
#else
#include <esp_err.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Snapshot trạng thái canonical của Node trong gateway.
 */
typedef struct {
    bool seen;
    bool desired_connected;
    uint32_t generation;
} tb_node_state_snapshot_t;

/**
 * @brief Khởi tạo module ThingsBoard REST Client (idempotent, an toàn khi gọi nhiều lần).
 * Tạo FreeRTOS background task (độ ưu tiên 3) chạy trên Core 0 để quản lý phiên
 * đăng nhập JWT và cập nhật thuộc tính SERVER_SCOPE (connected: true/false).
 */
esp_err_t tb_rest_client_init(void);

/**
 * @brief Cập nhật trạng thái kết nối mong muốn của Node vào bảng canonical (non-blocking, thread-safe, static memory).
 * Hoạt động độc lập và an toàn ngay cả trước khi tb_rest_client_init() chạy.
 *
 * @param node_id Chỉ số Node (0..9)
 * @param connected true: Đang kết nối | false: Đã ngắt kết nối
 */
void tb_rest_set_node_connected(uint8_t node_id, bool connected);

/**
 * @brief Suy luận trạng thái CONNECTED khi nhận telemetry chỉ khi Node chưa từng có trạng thái (seen == false).
 * Nếu Node đã có trạng thái (kể cả DISCONNECTED), hàm này không làm gì để tránh telemetry tồn đọng kích hoạt lại node.
 */
void tb_rest_infer_node_connected_if_unseen(uint8_t node_id);

/**
 * @brief Đọc snapshot trạng thái hiện tại của Node.
 * @return true nếu node_id hợp lệ và đã seen, false nếu chưa seen hoặc node_id ngoài dải (>= 10).
 */
bool tb_rest_get_node_snapshot(uint8_t node_id, tb_node_state_snapshot_t *out_snapshot);

/**
 * @brief Kiểm tra xem Node có cập nhật trạng thái cần đồng bộ lên MQTT không (mqtt_dirty == true).
 * @return true nếu cần sync và ghi thông tin vào out_snapshot, false nếu không cần.
 */
bool tb_rest_get_mqtt_pending_sync(uint8_t node_id, tb_node_state_snapshot_t *out_snapshot);

/**
 * @brief Xác nhận (ACK) MQTT đã đồng bộ thành công generation tương ứng.
 * Chỉ xóa cờ mqtt_dirty nếu generation khớp; không ảnh hưởng cờ rest_dirty.
 */
void tb_rest_ack_mqtt_synced(uint8_t node_id, uint32_t generation);

/**
 * @brief Đánh dấu tất cả các node seen cần được sync lại MQTT (dùng khi MQTT reconnect).
 */
void tb_rest_mark_all_mqtt_dirty(void);

/**
 * @brief Lấy task handle của tb_rest_task phục vụ giám sát stack watermark.
 */
void *tb_rest_get_task_handle(void);

/**
 * @brief Phân giải UUID từ JSON response ThingsBoard.
 */
bool tb_rest_parse_device_uuid(const char *json_str, char *out_uuid, size_t out_uuid_len);

/**
 * @brief Reset toàn bộ bảng trạng thái phục vụ unit test host.
 */
void tb_rest_reset_state_for_test(void);

#ifdef __cplusplus
}
#endif
