/* Standard C Libraries */
#include <string.h>

#ifndef HOST_UNIT_TEST
/* ESP-IDF System Headers */
#include <driver/uart.h>
#include <esp_err.h>
#include <esp_log.h>
#endif

/* Project Local Headers */
#include "uart_bridge_rx.h"
#include "uart_frame_def.h"

#ifndef HOST_UNIT_TEST
static const char *TAG = "uart_bridge";
#endif

void uart_bridge_fsm_init(bridge_fsm_t *fsm, uint64_t initial_epoch) {
    if (!fsm) return;
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = BRIDGE_LINK_UNSYNCED;
    fsm->epoch = initial_epoch;
}

void uart_bridge_fsm_reset(bridge_fsm_t *fsm) {
    if (!fsm) return;
    fsm->state = BRIDGE_LINK_UNSYNCED;
    fsm->snapshot_slots_mask = 0;
}

int uart_bridge_handle_hello(bridge_fsm_t *fsm,
                            const uart_ctrl_hello_pkt_t *hello,
                            uart_ctrl_sync_pkt_t *sync_out) {
    if (!fsm || !hello || !sync_out) {
        return -1;
    }

    if (hello->version < UART_BRIDGE_PROTOCOL_VERSION) {
        return -2; /* Incompatible version */
    }

    if (fsm->state == BRIDGE_LINK_UNSYNCED || hello->boot_nonce != fsm->central_boot_nonce) {
        /* New Central boot or first link synchronization */
        fsm->epoch++;
        fsm->central_boot_nonce = hello->boot_nonce;
        fsm->state = BRIDGE_LINK_SYNCED;
        fsm->snapshot_slots_mask = 0;
        fsm->sync_count++;
    }
    /* If already synced with identical boot nonce, maintain state and epoch (idempotent) */

    sync_out->version = UART_BRIDGE_PROTOCOL_VERSION;
    sync_out->boot_nonce = fsm->central_boot_nonce;
    sync_out->bridge_epoch = fsm->epoch;
    sync_out->status = 1;
    sync_out->reserved = 0;

    return 0;
}

int uart_bridge_request_snapshot(bridge_fsm_t *fsm,
                                 uart_snapshot_req_pkt_t *req_out) {
    if (!fsm || !req_out) {
        return -1;
    }
    if (fsm->state == BRIDGE_LINK_UNSYNCED) {
        return -2;
    }

    fsm->snapshot_tx_id++;
    fsm->snapshot_slots_mask = 0;

    req_out->bridge_epoch = fsm->epoch;
    req_out->transaction_id = fsm->snapshot_tx_id;

    return 0;
}

int uart_bridge_handle_snapshot_slot(bridge_fsm_t *fsm,
                                     uint8_t slot_id,
                                     uint64_t frame_epoch) {
    if (!fsm) {
        return -1;
    }
    if (fsm->state == BRIDGE_LINK_UNSYNCED) {
        return -2;
    }
    if (frame_epoch != fsm->epoch) {
        fsm->stale_epoch_drops++;
        return -3; /* Stale epoch barrier drop */
    }
    if (slot_id >= MAX_NODES_SUPPORTED) {
        return -4;
    }

    fsm->snapshot_slots_mask |= (uint16_t)(1U << slot_id);
    return 0;
}

int uart_bridge_handle_snapshot_end(bridge_fsm_t *fsm,
                                    const uart_snapshot_end_pkt_t *end) {
    if (!fsm || !end) {
        return -1;
    }
    if (fsm->state == BRIDGE_LINK_UNSYNCED) {
        return -2;
    }
    if (end->bridge_epoch != fsm->epoch) {
        fsm->stale_epoch_drops++;
        return -3;
    }
    if (end->transaction_id != fsm->snapshot_tx_id) {
        return -4; /* Mismatched transaction ID */
    }

    /* All 10 slots (0 .. 9) must have been received (mask = 0x03FF) */
    uint16_t expected_mask = (uint16_t)((1U << MAX_NODES_SUPPORTED) - 1U);
    if (fsm->snapshot_slots_mask != expected_mask || end->slot_count != MAX_NODES_SUPPORTED) {
        fsm->partial_snapshot_drops++;
        return -5; /* Incomplete snapshot rejected */
    }

    /* Snapshot successfully verified: commit and open ingest */
    fsm->state = BRIDGE_LINK_READY;
    return 0;
}

bool uart_bridge_is_ingest_allowed(const bridge_fsm_t *fsm, uint64_t frame_epoch) {
    if (!fsm) {
        return false;
    }
    if (fsm->state != BRIDGE_LINK_READY) {
        return false;
    }
    if (frame_epoch != fsm->epoch) {
        return false;
    }
    return true;
}

#ifndef HOST_UNIT_TEST
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

    err = uart_driver_install(UART_BRIDGE_PORT, UART_BRIDGE_BUF_SIZE * 2, UART_BRIDGE_BUF_SIZE * 2, 0, NULL, 0);
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

int uart_bridge_tx_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    uint8_t frame_buf[UART_MAX_FRAME_LEN];
    int frame_len = uart_frame_pack(frame_buf, sizeof(frame_buf), node_id, pkt_type, payload, len);
    if (frame_len < 0) {
        return frame_len;
    }
    int written = uart_write_bytes(UART_BRIDGE_PORT, (const char *)frame_buf, frame_len);
    return written;
}
#endif