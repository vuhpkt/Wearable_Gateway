#pragma once

/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>

/* ESP-IDF System Headers */
#include <driver/uart.h>
#include <esp_err.h>


/* Cấu hình phần cứng UART Bridge */
#define UART_BRIDGE_PORT        UART_NUM_1
#define UART_BRIDGE_RX_PIN      18
#define UART_BRIDGE_TX_PIN      17
#define UART_BRIDGE_BUF_SIZE    2048U

esp_err_t uart_bridge_rx_init(void);

int uart_bridge_rx_read(uint8_t *dest, size_t max_len, uint32_t timeout_ms);