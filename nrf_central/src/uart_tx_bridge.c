/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef HOST_TEST
/* Zephyr System Headers */
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#endif

/* Project Local Headers */
#include "uart_frame_def.h"
#include "uart_tx_bridge.h"
#include "ble_central_mgr.h"

#if (defined(HOST_TEST) || defined(HOST_UNIT_TEST)) && defined(__GNUC__)
__attribute__((weak)) int ble_central_mgr_write_cmd(uint8_t slot_id, const uint8_t *data, uint16_t len) {
    (void)slot_id;
    (void)data;
    (void)len;
    return 0;
}
#endif

#ifndef HOST_TEST
LOG_MODULE_REGISTER(uart_tx_bridge, LOG_LEVEL_INF);
#endif

/* ══════════════════════════════════════════════════════════════
 *  Central Bridge FSM Implementation
 * ════════════════════════════════════════════════════════════ */

void central_bridge_fsm_init(central_bridge_fsm_t *fsm, uint32_t boot_nonce) {
    if (!fsm) return;
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = CENTRAL_LINK_UNSYNCED;
    fsm->boot_nonce = boot_nonce;
}

int central_bridge_create_hello(central_bridge_fsm_t *fsm, uart_ctrl_hello_pkt_t *hello_out) {
    if (!fsm || !hello_out) return -1;
    hello_out->version = UART_BRIDGE_PROTOCOL_VERSION;
    hello_out->boot_nonce = fsm->boot_nonce;
    hello_out->reserved = 0;
    fsm->hello_sent_count++;
    return 0;
}

int central_bridge_handle_sync(central_bridge_fsm_t *fsm, const uart_ctrl_sync_pkt_t *sync) {
    if (!fsm || !sync) return -1;

    if (sync->version != UART_BRIDGE_PROTOCOL_VERSION) {
        return -2; /* Protocol version mismatch */
    }

    if (sync->boot_nonce != fsm->boot_nonce) {
        fsm->stale_sync_drops++;
        return -3; /* Stale sync from an earlier boot nonce */
    }

    if (sync->status != 1) {
        return -4; /* Sync rejected by peer */
    }

    fsm->active_epoch = sync->bridge_epoch;
    fsm->state = CENTRAL_LINK_SYNCED;
    fsm->sync_recv_count++;
    return 0;
}

int central_bridge_handle_snapshot_req(central_bridge_fsm_t *fsm, const uart_snapshot_req_pkt_t *req) {
    if (!fsm || !req) return -1;
    if (fsm->state != CENTRAL_LINK_SYNCED) {
        return -2;
    }
    if (req->bridge_epoch != fsm->active_epoch) {
        return -3; /* Epoch mismatch */
    }

    fsm->snapshot_tx_id = req->transaction_id;
    return 0;
}

int central_bridge_create_snapshot_end(central_bridge_fsm_t *fsm, uart_snapshot_end_pkt_t *end_out) {
    if (!fsm || !end_out) return -1;
    if (fsm->state != CENTRAL_LINK_SYNCED) {
        return -2;
    }

    end_out->bridge_epoch = fsm->active_epoch;
    end_out->transaction_id = fsm->snapshot_tx_id;
    end_out->slot_count = MAX_NODES_SUPPORTED;
    end_out->reserved = 0;
    return 0;
}

void central_bridge_barrier_reset(central_bridge_fsm_t *fsm) {
    if (!fsm) return;
    fsm->state = CENTRAL_LINK_UNSYNCED;
    fsm->active_epoch = 0;
    fsm->snapshot_tx_id = 0;
}

int central_bridge_dispatch_downlink(uint8_t node_id, const uint8_t *payload, uint8_t len) {
    if (node_id >= MAX_NODES_SUPPORTED || !payload || len == 0) {
        return -1;
    }
    return ble_central_mgr_write_cmd(node_id, payload, len);
}

/* ══════════════════════════════════════════════════════════════
 *  Zephyr EasyDMA Hardware Bridge Driver (Target Only)
 * ════════════════════════════════════════════════════════════ */
#ifndef HOST_TEST

#include <zephyr/random/random.h>
#include "ble_node_slots.h"
#include "uart_tx_queue.h"

#define UART_TX_BUF_SIZE 1024
#define UART_RX_BUF_SIZE 256

/*
 * EasyDMA buffers: Must reside in Data RAM (0x20000000 - 0x2003FFFF)
 * on Nordic nRF52840. Static allocation guarantees RAM residency.
 */
static uint8_t dma_tx_buf[UART_TX_BUF_SIZE];
static uint8_t s_rx_buf_0[UART_RX_BUF_SIZE];
static uint8_t s_rx_buf_1[UART_RX_BUF_SIZE];
static uint8_t *s_next_rx_buf = s_rx_buf_1;

/* Synchronization semaphore: Signaled when EasyDMA ENDTX event fires */
static struct k_sem tx_done_sem;

static struct k_mutex tx_mutex;
static central_bridge_fsm_t s_bridge_fsm;

static const struct device *const s_uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));

uint64_t uart_tx_bridge_get_active_epoch(void) {
    return s_bridge_fsm.active_epoch;
}

typedef enum {
    CENTRAL_RX_WAIT_SOF = 0,
    CENTRAL_RX_READ_NODE,
    CENTRAL_RX_READ_TYPE,
    CENTRAL_RX_READ_LEN,
    CENTRAL_RX_READ_PAYLOAD,
    CENTRAL_RX_CHECK_CRC,
} central_rx_state_t;

static central_rx_state_t s_rx_state = CENTRAL_RX_WAIT_SOF;
static uint8_t s_rx_node_id;
static uint8_t s_rx_pkt_type;
static uint8_t s_rx_len;
static uint8_t s_rx_idx;
static uint8_t s_rx_payload[UART_MAX_PAYLOAD_LEN];
static uint8_t s_rx_crc_buf[3 + UART_MAX_PAYLOAD_LEN];

static void central_dispatch_frame(uint8_t node_id, uint8_t pkt_type, const uint8_t *payload, uint8_t len) {
    (void)node_id;
    if (pkt_type == UART_PKT_TYPE_CONTROL_SYNC_REQ) {
        LOG_INF("🔄 Received SYNC_REQ from ESP32 -> re-broadcasting HELLO");
        uart_ctrl_hello_pkt_t hello;
        central_bridge_create_hello(&s_bridge_fsm, &hello);
        uart_tx_queue_send(0, UART_PKT_TYPE_CONTROL_HELLO, &hello, sizeof(hello));
        return;
    }

    if (pkt_type == UART_PKT_TYPE_CONTROL_SYNC) {
        if (len >= sizeof(uart_ctrl_sync_pkt_t)) {
            const uart_ctrl_sync_pkt_t *sync = (const uart_ctrl_sync_pkt_t *)payload;
            int ret = central_bridge_handle_sync(&s_bridge_fsm, sync);
            if (ret == 0) {
                LOG_INF("🤝 Central bridge SYNC established: epoch=%llu", (unsigned long long)sync->bridge_epoch);
            } else {
                LOG_WRN("⚠️ Central bridge SYNC rejected (err %d)", ret);
            }
        }
        return;
    }

    if (pkt_type == UART_PKT_TYPE_SNAPSHOT_REQ) {
        if (len >= sizeof(uart_snapshot_req_pkt_t)) {
            const uart_snapshot_req_pkt_t *req = (const uart_snapshot_req_pkt_t *)payload;
            if (central_bridge_handle_snapshot_req(&s_bridge_fsm, req) == 0) {
                LOG_INF("📋 Responding to snapshot request: tx_id=%u", (unsigned)req->transaction_id);
                for (uint8_t slot_idx = 0; slot_idx < MAX_NODES_SUPPORTED; slot_idx++) {
                    ble_node_slot_t *slot = ble_node_slot_get(slot_idx);
                    uint8_t status_buf[sizeof(uart_v2_envelope_t) + sizeof(uart_node_status_pkt_t)];
                    uart_v2_envelope_t *env = (uart_v2_envelope_t *)status_buf;
                    uart_node_status_pkt_t *st = (uart_node_status_pkt_t *)(status_buf + sizeof(uart_v2_envelope_t));

                    env->bridge_epoch = s_bridge_fsm.active_epoch;
                    env->session_id = (slot && slot->is_used) ? slot->session_id : 0;
                    env->seq = (slot && slot->is_used) ? slot->seq : 0;
                    env->central_rx_mono_ms = (uint64_t)k_uptime_get();

                    st->status = (slot && slot->is_used && slot->state >= NODE_STATE_CONNECTED) ? 1 : 0;
                    if (slot && slot->is_used) {
                        memcpy(st->mac, slot->mac_be, 6);
                    } else {
                        memset(st->mac, 0, 6);
                    }

                    uart_tx_queue_send(slot_idx, UART_PKT_TYPE_NODE_STATUS_V2, status_buf, sizeof(status_buf));
                }

                uart_snapshot_end_pkt_t end;
                if (central_bridge_create_snapshot_end(&s_bridge_fsm, &end) == 0) {
                    uart_tx_queue_send(0, UART_PKT_TYPE_SNAPSHOT_END, &end, sizeof(end));
                }
            }
        }
        return;
    }

    if (pkt_type == UART_PKT_TYPE_DOWNLINK) {
        LOG_INF("📥 Received DOWNLINK frame for slot %u (len %u)", (unsigned)node_id, (unsigned)len);
        int ret = ble_central_mgr_write_cmd(node_id, payload, len);
        if (ret != 0) {
            LOG_WRN("⚠️ Failed to dispatch downlink to slot %u (err %d)", (unsigned)node_id, ret);
        }
        return;
    }
}

static void central_parse_byte(uint8_t b) {
    switch (s_rx_state) {
    case CENTRAL_RX_WAIT_SOF:
        if (b == UART_FRAME_SOF) {
            s_rx_state = CENTRAL_RX_READ_NODE;
        }
        break;
    case CENTRAL_RX_READ_NODE:
        if (b < MAX_NODES_SUPPORTED) {
            s_rx_node_id = b;
            s_rx_crc_buf[0] = b;
            s_rx_state = CENTRAL_RX_READ_TYPE;
        } else {
            s_rx_state = (b == UART_FRAME_SOF) ? CENTRAL_RX_READ_NODE : CENTRAL_RX_WAIT_SOF;
        }
        break;
    case CENTRAL_RX_READ_TYPE:
        s_rx_pkt_type = b;
        s_rx_crc_buf[1] = b;
        s_rx_state = CENTRAL_RX_READ_LEN;
        break;
    case CENTRAL_RX_READ_LEN:
        if (b > 0 && b <= UART_MAX_PAYLOAD_LEN) {
            s_rx_len = b;
            s_rx_crc_buf[2] = b;
            s_rx_idx = 0;
            s_rx_state = CENTRAL_RX_READ_PAYLOAD;
        } else {
            s_rx_state = (b == UART_FRAME_SOF) ? CENTRAL_RX_READ_NODE : CENTRAL_RX_WAIT_SOF;
        }
        break;
    case CENTRAL_RX_READ_PAYLOAD:
        s_rx_payload[s_rx_idx] = b;
        s_rx_crc_buf[3 + s_rx_idx] = b;
        s_rx_idx++;
        if (s_rx_idx == s_rx_len) {
            s_rx_state = CENTRAL_RX_CHECK_CRC;
        }
        break;
    case CENTRAL_RX_CHECK_CRC: {
        uint8_t expected_crc = uart_crc8_calc(s_rx_crc_buf, 3 + s_rx_len);
        if (b == expected_crc) {
            central_dispatch_frame(s_rx_node_id, s_rx_pkt_type, s_rx_payload, s_rx_len);
        } else {
            LOG_WRN("UART RX CRC mismatch (calc 0x%02X != recv 0x%02X)", expected_crc, b);
        }
        s_rx_state = CENTRAL_RX_WAIT_SOF;
        break;
    }
    default:
        s_rx_state = CENTRAL_RX_WAIT_SOF;
        break;
    }
}

static void uart_async_callback(const struct device *dev, struct uart_event *evt, void *user_data) {
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

    case UART_RX_RDY:
        if (evt->data.rx.len > 0) {
            const uint8_t *buf = evt->data.rx.buf + evt->data.rx.offset;
            for (size_t i = 0; i < evt->data.rx.len; i++) {
                central_parse_byte(buf[i]);
            }
        }
        break;

    case UART_RX_BUF_REQUEST:
        /* Provide next buffer for continuous seamless DMA reception */
        uart_rx_buf_rsp(dev, s_next_rx_buf, UART_RX_BUF_SIZE);
        break;

    case UART_RX_BUF_RELEASED:
        s_next_rx_buf = evt->data.rx_buf.buf;
        break;

    case UART_RX_DISABLED:
        /* Re-enable double-buffered reception without flood logging (20ms = 20,000 us) */
        uart_rx_enable(dev, s_rx_buf_0, UART_RX_BUF_SIZE, 20000);
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

    /* Enable continuous double-buffered EasyDMA RX (20 ms = 20,000 us timeout) */
    ret = uart_rx_enable(s_uart_dev, s_rx_buf_0, sizeof(s_rx_buf_0), 20000);
    if (ret != 0) {
        LOG_WRN("UART async RX enable returned %d (will retry)", ret);
    }

    central_bridge_fsm_init(&s_bridge_fsm, sys_rand32_get());

    uart_ctrl_hello_pkt_t hello;
    central_bridge_create_hello(&s_bridge_fsm, &hello);
    uart_tx_bridge_send_frame(0, UART_PKT_TYPE_CONTROL_HELLO, &hello, sizeof(hello));

    LOG_INF("UART0 EasyDMA Bridge initialized at 460800 bps (TX: P0.06 | RX: P0.08) - Sent HELLO (nonce=0x%08X)", (unsigned)s_bridge_fsm.boot_nonce);
    return 0;
}

void uart_tx_bridge_poll_handshake(void) {
    if (s_bridge_fsm.state != CENTRAL_LINK_SYNCED) {
        static int64_t last_hello_mono = 0;
        int64_t now = k_uptime_get();
        if (now - last_hello_mono >= 2000) {
            last_hello_mono = now;
            uart_ctrl_hello_pkt_t hello;
            central_bridge_create_hello(&s_bridge_fsm, &hello);
            uart_tx_bridge_send_frame(0, UART_PKT_TYPE_CONTROL_HELLO, &hello, sizeof(hello));
            LOG_INF("🔄 Retrying bridge handshake: Sent HELLO (nonce=0x%08X)...", (unsigned)s_bridge_fsm.boot_nonce);
        }
    }
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

#endif