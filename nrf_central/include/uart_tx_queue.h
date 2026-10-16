#ifndef UART_TX_QUEUE_H
#define UART_TX_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "uart_frame_def.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_TX_QUEUE_DEPTH 32

/* Fixed-size flat item struct, aligned to 4 bytes */
typedef struct {
    uint8_t node_id;
    uint8_t pkt_type;
    uint8_t len;
    uint8_t payload[UART_MAX_PAYLOAD_LEN]; /* 128 bytes */
} uart_tx_item_t;

typedef struct {
    uint32_t queued_count;
    uint32_t sent_count;
    uint32_t dropped_count;
    uint32_t error_count;
} uart_tx_queue_stats_t;

/**
 * @brief Initialize the UART TX Queue module and clear stats.
 */
void uart_tx_queue_init(void);

/**
 * @brief Non-blocking producer call to enqueue a frame for transmission.
 *
 * @param node_id Target/Source node index (0..9)
 * @param pkt_type Packet type (UART_PKT_TYPE_*)
 * @param payload Pointer to payload buffer
 * @param len Payload length (1..128)
 * @return 0 on success, -ENOMEM if queue is full, -EINVAL if params invalid
 */
int uart_tx_queue_send(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len);

/**
 * @brief Retrieve an item from the queue (used by consumer worker).
 *
 * @param item Pointer to output item struct
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = forever)
 * @return 0 on success, negative error code otherwise
 */
int uart_tx_queue_get(uart_tx_item_t *item, int32_t timeout_ms);

/**
 * @brief Get queue statistics and current count of pending items.
 *
 * @param stats Output pointer to copy stats into (optional, can be NULL)
 * @return Current number of items waiting in queue
 */
uint32_t uart_tx_queue_get_stats(uart_tx_queue_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* UART_TX_QUEUE_H */
