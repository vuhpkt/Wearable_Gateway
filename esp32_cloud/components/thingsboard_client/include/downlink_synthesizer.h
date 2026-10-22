#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a 17-character ASCII MAC string ("XX:XX:XX:XX:XX:XX" or "XX-XX-XX-XX-XX-XX") into 6 bytes.
 * Handles uppercase, lowercase, and leading/trailing whitespace.
 * Rejects invalid characters, wrong lengths, broadcast (FF:FF:FF:FF:FF:FF), and all-zero MACs.
 *
 * @param mac_str Input string
 * @param mac_out Output 6-byte array
 * @return 0 on success, negative error code on failure.
 */
int mac_ascii_to_bytes(const char *mac_str, uint8_t mac_out[6]);

/**
 * @brief Zero-heap JSON Command Synthesizer.
 * Scans an attribute JSON string directly on static buffers without calling malloc or cJSON.
 * Synthesizes wire commands into out_buf matching ble_protocol_def.h.
 *
 * Supported commands:
 * - CMD_MODE_CFG (0xCB)
 * - CMD_VITAL_CFG (0xCC)
 * - CMD_PPG_CFG (0xCD)
 * - CMD_ECG_CFG (0xCF) (enforcing 250 Hz)
 * - CMD_THR (0xCE) (with float temp*10 conversion)
 * - CMD_NAME_CFG (0xC9) (max 15 chars)
 *
 * @param json_str    Input JSON string (e.g. ThingsBoard shared attributes payload)
 * @param out_buf     Buffer to receive wire command bytes
 * @param max_out_len Size of out_buf
 * @param out_len     Pointer to receive actual length of synthesized command
 * @return 0 on success, negative error code if no supported attributes found or invalid data.
 */
int downlink_synth_parse_json(const char *json_str, uint8_t *out_buf, uint8_t max_out_len, uint8_t *out_len);

/**
 * @brief Extracts the target device MAC string from ThingsBoard Gateway attribute wrapper:
 * {"device": "XX:XX:XX:XX:XX:XX", "data": { ... }}
 * and synthesizes the command from "data".
 *
 * @param json_str       Input gateway wrapper JSON string
 * @param device_out     Buffer to receive ASCII MAC string
 * @param max_device_len Size of device_out
 * @param cmd_out        Buffer to receive synthesized command bytes
 * @param max_cmd_len    Size of cmd_out
 * @param cmd_len_out    Pointer to receive synthesized command length
 * @return 0 on success, negative error code on failure.
 */
int downlink_synth_extract_device_and_cmd(const char *json_str,
                                          char *device_out, size_t max_device_len,
                                          uint8_t *cmd_out, uint8_t max_cmd_len, uint8_t *cmd_len_out);

#ifdef __cplusplus
}
#endif
