#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo module ThingsBoard REST Client.
 * Tạo FreeRTOS background task (độ ưu tiên 3) chạy trên Core 0 để quản lý phiên
 * đăng nhập JWT và cập nhật thuộc tính SERVER_SCOPE (connected: true/false).
 */
esp_err_t tb_rest_client_init(void);

/**
 * @brief Đưa yêu cầu cập nhật SERVER_SCOPE connected vào hàng đợi xử lý ngầm.
 * Non-blocking: Hàm đẩy lệnh vào queue và trả về ngay lập tức, không làm nghẽn UART/MQTT.
 *
 * @param node_id Chỉ số Node (0 ứng với Node1, 1 ứng với Node2,...)
 * @param connected true: Đang kết nối | false: Đã ngắt kết nối
 */
void tb_rest_set_node_connected(uint8_t node_id, bool connected);

/**
 * @brief Lấy task handle của tb_rest_task phục vụ giám sát stack watermark.
 */
void *tb_rest_get_task_handle(void);

/**
 * @brief Phân giải UUID từ JSON response ThingsBoard.
 */
bool tb_rest_parse_device_uuid(const char *json_str, char *out_uuid, size_t out_uuid_len);

#ifdef __cplusplus
}
#endif
