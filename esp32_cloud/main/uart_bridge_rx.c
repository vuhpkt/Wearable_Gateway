/* ESP-IDF System Headers */
#include <driver/uart.h>
#include <esp_err.h>
#include <esp_log.h>

/* Project Local Headers */
#include "uart_bridge_rx.h"
#include "uart_frame_def.h"

static const char *TAG = "uart_bridge";


esp_err_t uart_bridge_rx_init(void) {
    uart_config_t config = {
        .baud_rate = UART_BRIDGE_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,
    };

    esp_err_t err = uart_param_config(UART_BRIDGE_PORT, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(UART_BRIDGE_PORT, UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(UART_BRIDGE_PORT, UART_BRIDGE_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "UART1 Bridge initialized [Baud: %u | RX: GPIO%d | TX: GPIO%d | Buf: %uB]",
             UART_BRIDGE_BAUDRATE, UART_BRIDGE_RX_PIN, UART_BRIDGE_TX_PIN, UART_BRIDGE_BUF_SIZE);
    return ESP_OK;

}

int uart_bridge_rx_read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) {
    if (dest == NULL || max_len == 0) return -1;

    int rx_bytes = uart_read_bytes(UART_BRIDGE_PORT, dest, max_len, pdMS_TO_TICKS(timeout_ms));
    return rx_bytes; 
}