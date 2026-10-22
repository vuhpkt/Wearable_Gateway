#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "thingsboard_mqtt.h"
#include "data_synthesizer.h"
#include "downlink_synthesizer.h"
#include "uart_frame_def.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOUD_MQTT_EVT_CONNECTED = 1,
    CLOUD_MQTT_EVT_DISCONNECTED,
    CLOUD_MQTT_EVT_PUBACK,
} cloud_mqtt_evt_type_t;

typedef struct {
    cloud_mqtt_evt_type_t type;
    int                   msg_id;
} cloud_mqtt_evt_t;

#define CLOUD_UPLINK_EVT_QUEUE_SIZE 16

typedef struct {
    frame_snapshot_tuple_t tuple;
    bool                   has_pending;
} pending_announce_t;

typedef struct {
    mqtt_lifecycle_mgr_t   lifecycle;
    pending_announce_t     pending_announces[MAX_NODES_SUPPORTED];
    char                   tx_buffer[2048];
    char                   attr_buffer[512];
    
    /* Static ring buffer for MQTT event queue */
    cloud_mqtt_evt_t       evt_queue[CLOUD_UPLINK_EVT_QUEUE_SIZE];
    uint8_t                evt_head;
    uint8_t                evt_tail;
    uint8_t                evt_count;
    uint32_t               evt_overflow_count;
} cloud_uplink_t;

void cloud_uplink_init(cloud_uplink_t *uplink);

/* Non-blocking event enqueue (safe to call from MQTT event callback) */
bool cloud_uplink_post_mqtt_event(cloud_uplink_t *uplink, const cloud_mqtt_evt_t *evt);

/* Process queued MQTT events on Cloud Task (Core 0) */
void cloud_uplink_process_events(cloud_uplink_t *uplink);

/* Announce / Disconnect handling on Cloud Task */
void cloud_uplink_handle_node_status(cloud_uplink_t *uplink, const frame_snapshot_tuple_t *tuple, bool is_connected);

/* Telemetry dispatch handling on Cloud Task */
void cloud_uplink_process_telemetry(cloud_uplink_t *uplink, const frame_snapshot_tuple_t *tuple, uint64_t current_utc_ms);

/* Event notification helpers */
void cloud_uplink_on_mqtt_connected(cloud_uplink_t *uplink);
void cloud_uplink_on_mqtt_published(cloud_uplink_t *uplink, int msg_id);

/* Unified snapshot tuple dispatcher (status vs telemetry) */
void cloud_uplink_dispatch_tuple(cloud_uplink_t *uplink, const frame_snapshot_tuple_t *tuple);

/* MAC-to-Slot Lookup for Downlink commands */
int cloud_uplink_find_slot_by_mac(const cloud_uplink_t *uplink, const uint8_t target_mac[6]);

/* Downlink Attributes handler (translates TB JSON to UART 0x80 frame) */
int cloud_uplink_handle_downlink_attributes(cloud_uplink_t *uplink,
                                           const char *json_payload,
                                           uint8_t *out_uart_frame,
                                           size_t max_frame_len);

/* ══════════════════════════════════════════════════════════════
 *  Hybrid Facade APIs (Operating on default internal instance)
 * ════════════════════════════════════════════════════════════ */
void cloud_uplink_default_init(void);
void cloud_uplink_on_mqtt_connected_cb(void);
void cloud_uplink_on_mqtt_published_cb(int msg_id);
void cloud_uplink_default_process_events(void);
void cloud_uplink_default_dispatch(const frame_snapshot_tuple_t *tuple);
int  cloud_uplink_default_find_slot_by_mac(const uint8_t target_mac[6]);
cloud_uplink_t* cloud_uplink_get_default_instance(void);

#ifdef __cplusplus
}
#endif
