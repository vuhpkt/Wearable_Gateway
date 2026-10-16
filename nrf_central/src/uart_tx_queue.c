#include "uart_tx_queue.h"

#include <errno.h>
#include <string.h>

#ifdef HOST_TEST
#include "zephyr/kernel.h"
#else
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "uart_tx_bridge.h"

LOG_MODULE_REGISTER(uart_tx_queue, LOG_LEVEL_INF);
#endif

/* Message queue allocated statically in .bss (32 * 132B ≈ 4.2 KB) */
K_MSGQ_DEFINE(s_uart_tx_msgq, sizeof(uart_tx_item_t), UART_TX_QUEUE_DEPTH, 4);

/* Queue runtime statistics */
static uart_tx_queue_stats_t s_stats;

void uart_tx_queue_init(void) {
    k_msgq_purge(&s_uart_tx_msgq);
    memset(&s_stats, 0, sizeof(s_stats));
}

#define UART_TX_PRIORITY_RESERVE 4

int uart_tx_queue_send(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    if (!payload || len == 0 || len > UART_MAX_PAYLOAD_LEN || node_id >= MAX_NODES_SUPPORTED) {
        return -EINVAL;
    }

    /* M10: Reserve headroom for high-priority status frames so ECG bursts do not drop disconnect events */
    if (pkt_type == UART_PKT_TYPE_ECG_BATCH &&
        k_msgq_num_used_get(&s_uart_tx_msgq) >= (UART_TX_QUEUE_DEPTH - UART_TX_PRIORITY_RESERVE)) {
        s_stats.dropped_count++;
        return -ENOMEM;
    }

    uart_tx_item_t item;
    item.node_id = node_id;
    item.pkt_type = pkt_type;
    item.len = len;
    memcpy(item.payload, payload, len);

    int ret = k_msgq_put(&s_uart_tx_msgq, &item, K_NO_WAIT);
    if (ret != 0) {
        s_stats.dropped_count++;
        return -ENOMEM;
    }

    s_stats.queued_count++;
    return 0;
}

int uart_tx_queue_get(uart_tx_item_t *item, int32_t timeout_ms) {
    if (!item) {
        return -EINVAL;
    }

    k_timeout_t timeout;
    if (timeout_ms < 0) {
        timeout = K_FOREVER;
    } else if (timeout_ms == 0) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_MSEC(timeout_ms);
    }

    return k_msgq_get(&s_uart_tx_msgq, item, timeout);
}

uint32_t uart_tx_queue_get_stats(uart_tx_queue_stats_t *stats) {
    if (stats) {
        *stats = s_stats;
    }
    return k_msgq_num_used_get(&s_uart_tx_msgq);
}

/* ══════════════════════════════════════════════════════════════
 *  Dedicated Consumer Worker Thread (Zephyr RTOS)
 *  Priority: 9 (Preemptible by BT RX Host thread priority 8)
 * ══════════════════════════════════════════════════════════════ */
#ifndef HOST_TEST
#define UART_TX_WORKER_STACK_SIZE 2048
#define UART_TX_WORKER_PRIORITY   K_PRIO_PREEMPT(9)

static void uart_tx_worker_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    uart_tx_item_t item;
    LOG_INF("UART TX Worker Thread started (Priority: %d, Stack: %u B)",
            UART_TX_WORKER_PRIORITY, UART_TX_WORKER_STACK_SIZE);

    while (1) {
        int ret = k_msgq_get(&s_uart_tx_msgq, &item, K_FOREVER);
        if (ret == 0) {
            int tx_ret = uart_tx_bridge_send_frame(item.node_id, item.pkt_type, item.payload, item.len);
            if (tx_ret == 0) {
                s_stats.sent_count++;
            } else {
                s_stats.error_count++;
                LOG_ERR("UART bridge send frame failed (err %d)", tx_ret);
            }
        }
    }
}

K_THREAD_DEFINE(uart_tx_worker_tid, UART_TX_WORKER_STACK_SIZE,
                uart_tx_worker_entry, NULL, NULL, NULL,
                UART_TX_WORKER_PRIORITY, 0, 0);
#endif
