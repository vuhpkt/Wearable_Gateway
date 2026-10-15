#pragma once

/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>


int uart_tx_bridge_init(void);

int uart_tx_bridge_send(const uint8_t *data, size_t len);

int uart_tx_bridge_send_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len);