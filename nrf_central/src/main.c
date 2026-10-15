
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "ble_central_mgr.h"
#include "ble_protocol_def.h"
#include "uart_frame_def.h"
#include "uart_tx_bridge.h"
#include "uart_tx_queue.h"

LOG_MODULE_REGISTER(gateway_main, LOG_LEVEL_INF);

static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define LED_BLINK_INTERVAL_MS 1000

static void on_ble_connected(uint8_t node_id, const bt_addr_le_t *addr) {
    uint8_t status = 1; /* 1 = Connected */
    uart_tx_queue_send(node_id, UART_PKT_TYPE_NODE_STATUS, &status, sizeof(status));
}

static void on_ble_disconnected(uint8_t node_id, const bt_addr_le_t *addr, uint8_t reason) {
    uint8_t status = 0; /* 0 = Disconnected */
    uart_tx_queue_send(node_id, UART_PKT_TYPE_NODE_STATUS, &status, sizeof(status));
}

static void on_vitals_received(uint8_t node_id, const vitals_packet_t *vitals) {
    uart_tx_queue_send(node_id, UART_PKT_TYPE_VITALS, vitals, sizeof(vitals_packet_t));
}

static void on_ecg_received(uint8_t node_id, const int16_t *samples, uint16_t count) {
    uint8_t byte_len = (uint8_t)(count * sizeof(int16_t));
    if (byte_len > UART_MAX_PAYLOAD_LEN) {
        byte_len = UART_MAX_PAYLOAD_LEN;
    }
    uart_tx_queue_send(node_id, UART_PKT_TYPE_ECG_BATCH, samples, byte_len);
}

static void on_raw_received(uint8_t node_id, const uint8_t *data, uint16_t len) {
    ARG_UNUSED(node_id);
    ARG_UNUSED(data);
    ARG_UNUSED(len);
}

static const ble_central_callbacks_t ble_callbacks = {
    .connected = on_ble_connected,
    .disconnected = on_ble_disconnected,
    .vitals_received = on_vitals_received,
    .ecg_received = on_ecg_received,
    .raw_received = on_raw_received,
};

/* ══════════════════════════════════════════════════════════════
 *  Main Application Entry Point
 * ════════════════════════════════════════════════════════════ */
int main(void) {
    int ret;
    bool led_state = false;
    uint32_t heartbeat_cnt = 0;

    if (gpio_is_ready_dt(&user_led)) {
        gpio_pin_configure_dt(&user_led, GPIO_OUTPUT_ACTIVE);
    }

    printk("\n==================================================\n");
    printk("  [Wearable Gateway] Zephyr BLE Central & Bridge    \n");
    printk("  Board: Pro Micro nRF52840 (UF2)                  \n");
    printk("  Inter-Chip UART Bridge @ 460800 bps              \n");
    printk("==================================================\n\n");

    ret = uart_tx_bridge_init();
    if (ret != 0) {
        LOG_ERR("UART Bridge init failed (%d)", ret);
    }

    uart_tx_queue_init();

    ret = ble_central_mgr_init(&ble_callbacks);
    if (ret != 0) {
        LOG_ERR("BLE Central Manager init failed (err %d)", ret);
    } else {
        LOG_INF("BLE Central Manager initialized. Waiting for '%s'...", BLE_DEV_NAME_PREFIX);
    }

    while (1) {
        gpio_pin_toggle_dt(&user_led);
        led_state = !led_state;

        uart_tx_queue_stats_t q_stats;
        uint32_t q_pending = uart_tx_queue_get_stats(&q_stats);

        LOG_INF("[HEARTBEAT #%03u] Nodes: %u/10 (Sub: %u) | Q: %u/32 (Sent: %u, Drop: %u) | Uptime: %lld ms",
                heartbeat_cnt++,
                ble_central_mgr_get_connected_count(),
                ble_central_mgr_get_subscribed_count(),
                q_pending,
                q_stats.sent_count,
                q_stats.dropped_count,
                k_uptime_get());

        k_msleep(LED_BLINK_INTERVAL_MS);
    }


    return 0;
}