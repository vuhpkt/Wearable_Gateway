
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

static void on_ble_connected(const ble_conn_context_t *ctx, const bt_addr_le_t *addr) {
    ARG_UNUSED(addr);
    uint8_t payload[sizeof(uart_v2_envelope_t) + sizeof(uart_node_status_pkt_t)];
    uart_v2_envelope_t *env = (uart_v2_envelope_t *)payload;
    uart_node_status_pkt_t *status = (uart_node_status_pkt_t *)(payload + sizeof(uart_v2_envelope_t));

    env->bridge_epoch = uart_tx_bridge_get_active_epoch();
    env->session_id = ctx->session_id;
    env->seq = ctx->seq;
    env->central_rx_mono_ms = ctx->rx_mono_ms;

    status->status = 1; /* 1 = Connected */
    memcpy(status->mac, ctx->mac, 6);

    uart_tx_queue_send(ctx->slot_id, UART_PKT_TYPE_NODE_STATUS_V2, payload, sizeof(payload));
}

static void on_ble_disconnected(const ble_conn_context_t *ctx, const bt_addr_le_t *addr, uint8_t reason) {
    ARG_UNUSED(addr);
    ARG_UNUSED(reason);
    uint8_t payload[sizeof(uart_v2_envelope_t) + sizeof(uart_node_status_pkt_t)];
    uart_v2_envelope_t *env = (uart_v2_envelope_t *)payload;
    uart_node_status_pkt_t *status = (uart_node_status_pkt_t *)(payload + sizeof(uart_v2_envelope_t));

    env->bridge_epoch = uart_tx_bridge_get_active_epoch();
    env->session_id = ctx->session_id;
    env->seq = ctx->seq;
    env->central_rx_mono_ms = ctx->rx_mono_ms;

    status->status = 0; /* 0 = Disconnected */
    memcpy(status->mac, ctx->mac, 6);

    uart_tx_queue_send(ctx->slot_id, UART_PKT_TYPE_NODE_STATUS_V2, payload, sizeof(payload));
}

static void on_vitals_received(const ble_conn_context_t *ctx, const vitals_packet_t *vitals) {
    uint8_t payload[sizeof(uart_v2_envelope_t) + sizeof(vitals_packet_t)];
    uart_v2_envelope_t *env = (uart_v2_envelope_t *)payload;

    env->bridge_epoch = uart_tx_bridge_get_active_epoch();
    env->session_id = ctx->session_id;
    env->seq = ctx->seq;
    env->central_rx_mono_ms = ctx->rx_mono_ms;

    memcpy(payload + sizeof(uart_v2_envelope_t), vitals, sizeof(vitals_packet_t));
    uart_tx_queue_send(ctx->slot_id, UART_PKT_TYPE_VITALS_V2, payload, sizeof(payload));
}

static void on_ecg_received(const ble_conn_context_t *ctx, const int16_t *samples, uint16_t count) {
    uint8_t sample_bytes = (uint8_t)(count * sizeof(int16_t));
    if (sample_bytes > 100) {
        sample_bytes = 100;
    }

    uint8_t payload[sizeof(uart_v2_envelope_t) + 100];
    uart_v2_envelope_t *env = (uart_v2_envelope_t *)payload;

    env->bridge_epoch = uart_tx_bridge_get_active_epoch();
    env->session_id = ctx->session_id;
    env->seq = ctx->seq;
    env->central_rx_mono_ms = ctx->rx_mono_ms;

    memcpy(payload + sizeof(uart_v2_envelope_t), samples, sample_bytes);
    uart_tx_queue_send(ctx->slot_id, UART_PKT_TYPE_ECG_V2, payload, (uint8_t)(sizeof(uart_v2_envelope_t) + sample_bytes));
}

static void on_raw_received(const ble_conn_context_t *ctx, const uint8_t *data, uint16_t len) {
    ARG_UNUSED(ctx);
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

        uart_tx_bridge_poll_handshake();

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