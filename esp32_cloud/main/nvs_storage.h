#pragma once

/* Standard C Libraries */
#include <stdbool.h>
#include <stddef.h>

/* ESP-IDF System Headers */
#include <esp_err.h>


#define MAX_SSID_LEN        32
#define MAX_PASSWORD_LEN    64


/**
 * @brief Khởi tạo phân vùng NVS flash
 */
void nvs_storage_init(void);

/**
 * @brief Đọc thông tin Wi-Fi đã lưu từ NVS
 * @return ESP_OK nếu đọc thành công, ESP_ERR_NVS_NOT_FOUND nếu chưa từng lưu
 */
esp_err_t nvs_storage_read_wifi(char *ssid, size_t ssid_size, char *password, size_t pass_size);

/**
 * @brief Lưu thông tin Wi-Fi mới vào NVS (kèm nvs_commit)
 */
esp_err_t nvs_storage_save_wifi(const char *ssid, const char *password);

