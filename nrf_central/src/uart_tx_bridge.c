/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Zephyr System Headers */
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Project Local Headers */
#include "uart_frame_def.h"
#include "uart_tx_bridge.h"

LOG_MODULE_REGISTER(uart_tx_bridge, LOG_LEVEL_INF);

#define UART_TX_BUF_SIZE 1024
/*
 * EasyDMA buffer: Must reside in Data RAM (0x20000000 - 0x2003FFFF)
 * on Nordic nRF52840. Static allocation guarantees RAM residency.
 */
static uint8_t dma_tx_buf[UART_TX_BUF_SIZE];

/* Synchronization semaphore: Signaled when EasyDMA ENDTX event fires */
static struct k_sem tx_done_sem;

/* Mutex protecting shared DMA buffer across threads */
static struct k_mutex tx_mutex;

static const struct device *const s_uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));


static void uart_async_callback(const struct device *dev, struct uart_event *evt, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case UART_TX_DONE:
        LOG_DBG("UART EasyDMA TX Complete (%zu bytes)", evt->data.tx.len);
        k_sem_give(&tx_done_sem);
        break;
    case UART_TX_ABORTED:
        LOG_WRN("UART EasyDMA TX Aborted");
        k_sem_give(&tx_done_sem);
        break;
    default:
        break;
    }
}

int uart_tx_bridge_init(void) {
    if (!device_is_ready(s_uart_dev)) {
        LOG_ERR("UART0 device is NOT ready!");
        return -ENODEV;
    }

    k_mutex_init(&tx_mutex);
    k_sem_init(&tx_done_sem, 0, 1);
    int ret = uart_callback_set(s_uart_dev, uart_async_callback, NULL);
    if (ret != 0) {
        LOG_ERR("Failed to set UART async callback: %d", ret);
        return ret;
    }

    LOG_INF("UART0 EasyDMA Bridge initialized at 460800 bps (TX: P0.06 | RX: P0.08)");
    return 0;
}

int uart_tx_bridge_send(const uint8_t *data, size_t len) {
    if (!device_is_ready(s_uart_dev) || data == NULL || len == 0 || len > UART_TX_BUF_SIZE) {
        return -EINVAL;
    }

    k_mutex_lock(&tx_mutex, K_FOREVER);

    memcpy(dma_tx_buf, data, len);

    int ret = uart_tx(s_uart_dev, dma_tx_buf, len, SYS_FOREVER_US);
    if (ret < 0) {
        LOG_ERR("uart_tx failed: %d", ret);
        k_mutex_unlock(&tx_mutex);
        return ret;
    }

    ret = k_sem_take(&tx_done_sem, K_MSEC(50));
    if (ret != 0) {
        LOG_ERR("UART EasyDMA TX timeout, aborting transmission");
        uart_tx_abort(s_uart_dev);
    }

    k_mutex_unlock(&tx_mutex);
    return ret;
}

int uart_tx_bridge_send_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    k_mutex_lock(&tx_mutex, K_FOREVER);

    int frame_len = uart_frame_pack(dma_tx_buf, sizeof(dma_tx_buf), node_id, pkt_type, payload, len);
    if (frame_len < 0) {
        LOG_ERR("uart_frame_pack failed: %d (node=%u, type=%u, len=%u)", frame_len, node_id, pkt_type, len);
        k_mutex_unlock(&tx_mutex);
        return -EINVAL;
    }

    int ret = uart_tx(s_uart_dev, dma_tx_buf, frame_len, SYS_FOREVER_US);
    if (ret < 0) {
        LOG_ERR("uart_tx failed: %d", ret);
        k_mutex_unlock(&tx_mutex);
        return ret;
    }

    ret = k_sem_take(&tx_done_sem, K_MSEC(100));
    if (ret != 0) {
        LOG_ERR("UART EasyDMA TX timeout, aborting transmission");
        uart_tx_abort(s_uart_dev);
    }

    k_mutex_unlock(&tx_mutex);
    return ret;
}