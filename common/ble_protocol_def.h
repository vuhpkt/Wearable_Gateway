#ifndef BLE_PROTOCOL_DEF_H
#define BLE_PROTOCOL_DEF_H

#include <stdint.h>
#include <stdbool.h>

/* ══════════════════════════════════════════════════════════════
 *  BLE GATT Service & Characteristic UUIDs
 *  Base UUID: 6E40xxxx-B5A3-F393-E0A9-E50E24DCCA9E
 * ════════════════════════════════════════════════════════════ */
#define BLE_CUS_BASE_UUID_128   { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, \
                                  0x93, 0xF3, 0xA3, 0xB5, 0x00, 0x00, 0x40, 0x6E }

#define BLE_CUS_SERVICE_UUID           0x1400  /* Custom Primary Service */
#define BLE_CUS_TX_CHAR_UUID           0x1401  /* Node -> Central (Notify) */
#define BLE_CUS_RX_CHAR_UUID           0x1402  /* Central -> Node (Write)  */

#define BLE_DEV_NAME_PREFIX            "ECG_dev"

/* ══════════════════════════════════════════════════════════════
 *  BLE Packet Lengths & Timings
 * ════════════════════════════════════════════════════════════ */
#define ECG_SAMPLE_RATE_HZ             250U    /* 250 Hz SAADC PPI sampling */
#define ECG_SAMPLE_INTERVAL_MS         4U      /* 4 ms per ECG sample */

#define ECG_DEFAULT_SAMPLES_PER_PKT    50U     /* 50 samples = 200 ms batch */
#define ECG_DEFAULT_PAYLOAD_LEN        (ECG_DEFAULT_SAMPLES_PER_PKT * sizeof(int16_t)) /* 100 bytes */

#define VITALS_PAYLOAD_LEN             5U      /* Exactly 5 bytes */

/* ══════════════════════════════════════════════════════════════
 *  Vitals Payload Layout (5 Bytes)
 *  Byte 0: ECG Heart Rate (uint8_t, bpm)
 *  Byte 1: PPG Heart Rate (uint8_t, bpm)
 *  Byte 2: SpO2 (uint8_t, %)
 *  Byte 3: Temperature Low Byte  (uint16_t LE, temp x 10)
 *  Byte 4: Temperature High Byte
 * ════════════════════════════════════════════════════════════ */
#pragma pack(push, 1)
typedef struct {
    uint8_t  hr_ecg;
    uint8_t  hr_ppg;
    uint8_t  spo2;
    uint16_t temp_x10;  /* temp_celsius = temp_x10 / 10.0f */
} vitals_packet_t;
#pragma pack(pop)

/* ══════════════════════════════════════════════════════════════
 *  Downlink Commands (Gateway -> Node via RX Char 0x1402)
 * ════════════════════════════════════════════════════════════ */
#define CMD_ECG_CFG                    0xCF    /* 5 bytes: [0xCF][freq_lo][freq_hi][int_lo][int_hi] */
#define CMD_THR                        0xCE    /* 31 bytes: alert thresholds */
#define CMD_PPG_CFG                    0xCD    /* 3-4 bytes: [0xCD][freq_lo][freq_hi][hr_src] */
#define CMD_VITAL_CFG                  0xCC    /* 3-5 bytes: [0xCC][int_lo][int_hi][lcd_lo][lcd_hi] */
#define CMD_MODE_CFG                   0xCB    /* 7 bytes: mode, period, capture, ecgEnabled */
#define CMD_NAME_CFG                   0xC9    /* 2-17 bytes: [0xC9][len][name...] */

#endif /* BLE_PROTOCOL_DEF_H */
