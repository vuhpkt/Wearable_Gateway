#pragma once

/* Standard C Libraries */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Project Local Headers */
#include "ble_protocol_def.h"
#include "frame_snapshot.h"

#define SYNTH_SAMPLE_INTERVAL_MS    4U   /* 4 ms / sample @ 250 Hz */
#define DEFAULT_GATEWAY_ID          "gw-01"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Format MAC-native connection message for ThingsBoard Gateway API
 * Topic: "v1/gateway/connect"
 * Payload: {"device":"E4:5F:01:23:45:67","type":"Wearable"}
 */
int synth_format_mac_connect(char *dest, size_t max_len, const uint8_t *mac, const char **out_topic);

/**
 * @brief Format MAC-native disconnection message for ThingsBoard Gateway API
 * Topic: "v1/gateway/disconnect"
 * Payload: {"device":"E4:5F:01:23:45:67"}
 */
int synth_format_mac_disconnect(char *dest, size_t max_len, const uint8_t *mac, const char **out_topic);

/**
 * @brief Format MAC-native CLIENT_SCOPE attributes message
 * Topic: "v1/gateway/attributes"
 */
int synth_format_mac_attributes(char *dest, size_t max_len,
                               const frame_snapshot_tuple_t *tuple,
                               bool ble_connected,
                               const char *gateway_id);

/**
 * @brief Format MAC-native vitals telemetry message
 * Topic: "v1/gateway/telemetry"
 * Returns length on success, 0 if all metrics are 0, -2 if unsynchronized time, -1 on buffer error.
 */
int synth_format_mac_vitals(char *dest, size_t max_len,
                           const frame_snapshot_tuple_t *tuple,
                           const vitals_packet_t *v,
                           const char *gateway_id);

/**
 * @brief Format MAC-native ECG batch telemetry message
 * Topic: "v1/gateway/telemetry"
 * Returns length on success, -2 if unsynchronized time, -1 on buffer error.
 */
int synth_format_mac_ecg_batch(char *dest, size_t max_len,
                              const frame_snapshot_tuple_t *tuple,
                              const int16_t *samples,
                              uint16_t count,
                              const char *gateway_id);

/* Legacy Node-ID APIs (preserved for backward compatibility) */
int synth_format_ecg_batch(char *dest, size_t max_len, uint8_t node_id, 
                           const int16_t *samples, uint16_t count);

int synth_format_vitals(char *dest, size_t max_len, uint8_t node_id, 
                        const vitals_packet_t *v);

int synth_format_node_status(char *dest, size_t max_len, uint8_t node_id, 
                             bool is_connected, const char **out_topic);

#ifdef __cplusplus
}
#endif
