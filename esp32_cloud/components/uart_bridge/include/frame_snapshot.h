#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "uart_frame_def.h"
#include "bridge_time.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#define FRAME_SNAPSHOT_QUEUE_CAPACITY     64U
#define FRAME_SNAPSHOT_PRIORITY_CAPACITY  16U

typedef enum {
    STREAM_TYPE_STATUS = 1,
    STREAM_TYPE_VITALS = 2,
    STREAM_TYPE_ECG    = 3,
} stream_type_t;

/* Backward-compatible aliases */
#define TIME_QUAL_UNCALIBRATED TIME_QUALITY_UNKNOWN
#define TIME_QUAL_SYNCED       TIME_QUALITY_SYNCED

#pragma pack(push, 1)
typedef struct {
    uint8_t  mac[6];            /* Canonical Big-Endian MAC display order */
    uint32_t session_id;        /* Active connection session ID */
    uint64_t timestamp_utc_ms;  /* UTC epoch ms (0 if uncalibrated) */
    uint64_t bridge_epoch;      /* Current bridge epoch */
    uint32_t seq;               /* Monotonic packet sequence */
    uint8_t  node_id;           /* Slot index (0 .. 9) */
    uint8_t  stream_type;       /* stream_type_t */
    uint64_t central_rx_mono_ms;/* Monotonic ms when Central received packet */
    uint64_t esp_rx_mono_ms;    /* Monotonic ms when ESP received packet */
    uint8_t  time_quality;      /* time_quality_t */
    uint16_t uncertainty_ms;    /* Estimated uncertainty in ms */
    uint8_t  payload_len;       /* Payload byte length (1 .. 100) */
    uint8_t  payload[100];      /* Deep copy of payload bytes */
} frame_snapshot_tuple_t;
#pragma pack(pop)

typedef struct {
    frame_snapshot_tuple_t items[FRAME_SNAPSHOT_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t dropped_count;

    /* Priority mailbox for control/status frames */
    frame_snapshot_tuple_t priority_items[FRAME_SNAPSHOT_PRIORITY_CAPACITY];
    uint32_t prio_head;
    uint32_t prio_tail;
    uint32_t prio_count;
    uint32_t prio_dropped_count;

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_TYPE mux;
    SemaphoreHandle_t sem_items;
    StaticSemaphore_t sem_buffer;
#endif
} frame_snapshot_queue_t;

/* Revocation table with Epoch Guard */
typedef struct {
    uint64_t active_epoch[MAX_NODES_SUPPORTED];
    uint32_t active_session[MAX_NODES_SUPPORTED];
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_TYPE rev_mux;
#endif
} revocation_table_t;

typedef struct {
    uint32_t count;
    uint32_t dropped_count;
    uint32_t prio_count;
    uint32_t prio_dropped_count;
} frame_snapshot_queue_stats_t;

void frame_snapshot_queue_init(frame_snapshot_queue_t *q);
bool frame_snapshot_queue_push(frame_snapshot_queue_t *q, const frame_snapshot_tuple_t *item);
bool frame_snapshot_queue_push_priority(frame_snapshot_queue_t *q, const frame_snapshot_tuple_t *item);
bool frame_snapshot_queue_pop(frame_snapshot_queue_t *q, frame_snapshot_tuple_t *item_out, uint32_t timeout_ms);
uint32_t frame_snapshot_queue_count(const frame_snapshot_queue_t *q);
uint32_t frame_snapshot_queue_dropped(const frame_snapshot_queue_t *q);
uint32_t frame_snapshot_queue_prio_dropped(const frame_snapshot_queue_t *q);
void frame_snapshot_queue_get_stats(const frame_snapshot_queue_t *q, frame_snapshot_queue_stats_t *out_stats);

void revocation_table_init(revocation_table_t *tbl);
void revocation_table_set_active_session(revocation_table_t *tbl, uint8_t node_id, uint64_t bridge_epoch, uint32_t session_id);
bool revocation_table_is_session_valid(const revocation_table_t *tbl, uint8_t node_id, uint64_t bridge_epoch, uint32_t session_id);
bool revocation_table_revoke_slot(revocation_table_t *tbl, uint8_t node_id, uint64_t bridge_epoch, uint32_t session_id);
void revocation_table_reset_epoch(revocation_table_t *tbl, uint64_t new_epoch);

/* Aliases */
#define frame_snapshot_queue_push_control frame_snapshot_queue_push_priority
#define revocation_table_bind_session revocation_table_set_active_session
