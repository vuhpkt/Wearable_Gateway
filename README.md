# Wearable Health Monitor — Dual-MCU Gateway

[![Gateway Host Unit Tests CI](https://github.com/quang-nguyen-161/Wearable_Gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/quang-nguyen-161/Wearable_Gateway/actions/workflows/ci.yml)

A high-throughput, low-latency **Dual-MCU IoT Medical Gateway** integrating **nRF52840** (BLE 5.0 Central) and **ESP32-S3** (Wi-Fi / ThingsBoard MQTT) designed for wearable patient health telemetry (ECG, PPG SpO2, Heart Rate, Temperature, Battery).

---

## 1. System Architecture

```
                    ┌─────────────────────────┐
                    │  Wearable Medical Nodes │ (Up to 10 nodes)
                    │  (AD8232 ECG, MAX30102) │
                    └───────────┬─────────────┘
                                │ BLE 5.0 GATT (250 Hz ECG, 1 Hz Vitals)
                                ▼
               ┌─────────────────────────────────┐
               │    Central MCU: Nordic nRF52840  │
               │         (Zephyr RTOS)           │
               └────────────────┬────────────────┘
                                │ Physical 3-Wire UART (460,800 baud)
                                │ nRF P0.06 (TX) ──> ESP32 GPIO18 (RX)
                                │ nRF P0.08 (RX) <── ESP32 GPIO17 (TX)
                                │ Common GND
                                ▼
               ┌─────────────────────────────────┐
               │    Cloud MCU: Espressif ESP32-S3│
               │      (ESP-IDF v6.0.2 / FreeRTOS)│
               └────────────────┬────────────────┘
                                │ MQTT over TCP/TLS (v1/gateway/*)
                                ▼
               ┌─────────────────────────────────┐
               │     ThingsBoard IoT Server      │
               │   (Live Clinical Dashboard)     │
               └─────────────────────────────────┘
```

### Core Architecture Highlights
- **Zero-Heap Memory Contract:** Static allocation across both MCUs (`malloc`/`free` prohibited in runtime loops).
- **Architecture v2 MAC-Native Reporting:** Leaf devices are mapped dynamically by Bluetooth MAC (`XX:XX:XX:XX:XX:XX`) to native ThingsBoard entities on `v1/gateway/connect`, `v1/gateway/telemetry`, `v1/gateway/attributes`, and `v1/gateway/disconnect`.
- **Bidirectional Downlink Pipeline:** Zero-heap downlink synthesizer parses ThingsBoard JSON commands (`CMD_MODE_CFG`, `CMD_TIME_CFG`, `CMD_ECG_CFG`) and transmits 7-byte binary frames to the wearable nodes.
- **Clock Synchronization:** Monotonic Zephyr timestamp on nRF52840 is dynamically mapped to UTC milliseconds on ESP32-S3 via pool.ntp.org SNTP, automatically pushing real-time clock downlinks to watches.
- **Fail-Safe Wi-Fi Provisioning:** Captive portal with dynamic SoftAP $\leftrightarrow$ Station FSM transitions (no `esp_restart()` to avoid dropping in-flight patient telemetry).

---

## 2. Directory Layout

```
Wearable_Gateway/
├── common/                  # Shared protocol definitions & binary framing contracts
│   ├── ble_protocol_def.h   # BLE GATT UUIDs, packet structures, and wire commands
│   ├── uart_frame_def.h     # UART framing (SOF 0xAA55, types, CRC-8, ACK/NACK)
│   └── CONTRACT_HASH.sha256 # Cryptographic contract hash verification
├── esp32_cloud/             # ESP32-S3 Cloud Uplink Firmware (ESP-IDF v6.0.2)
│   ├── components/
│   │   ├── uart_bridge/     # Core 0 UART streaming receiver & Core 1 ingestion gate
│   │   ├── thingsboard_client/ # MQTT Gateway lifecycle, JSON synthesizer, Outbox
│   │   └── wifi_manager/    # Captive portal, STA reconnect backoff, NVS storage
│   ├── main/                # Zero-heap composition root & system health task
│   └── CMakeLists.txt
├── nrf_central/             # nRF52840 Central Collector Firmware (Zephyr RTOS)
│   ├── src/
│   │   ├── ble_central_mgr.c # Active BLE scanner, auto-connect, MTU exchange
│   │   ├── ble_node_slots.c  # 10-node slot management & session isolation
│   │   ├── uart_tx_bridge.c  # Downlink UART receiver & frame dispatcher
│   │   └── uart_tx_queue.c   # Lock-free UART TX ring buffer
│   ├── prj.conf             # Zephyr configuration (Bluetooth Central, UART)
│   └── CMakeLists.txt
├── tests/                   # Host-based Unit Testing Suite (GCC TDD)
│   ├── contracts/           # Telemetry & fault injection test contracts
│   ├── mocks/               # FreeRTOS, ESP-IDF, and Zephyr host port mocks
│   └── test_*.c             # 28 comprehensive host unit test suites
├── tools/
│   ├── run_tests.sh         # CI & local GCC test execution script
│   └── mock_server/         # Synthetic MQTT / TCP test utility
├── docs/                    # Architecture Specifications & API documentation
├── .github/workflows/ci.yml # GitHub Actions CI automation
└── README.md
```

---

## 3. Hardware Interconnect & Pin Mapping

Connect the two MCUs using a 3-wire UART link:

| Signal | Nordic nRF52840 | Espressif ESP32-S3 | Notes |
|---|---|---|---|
| **Uplink (TX $\rightarrow$ RX)** | `P0.06` (TX) | `GPIO 18` (RX) | 460,800 baud, 8N1 |
| **Downlink (RX $\leftarrow$ TX)** | `P0.08` (RX) | `GPIO 17` (TX) | 460,800 baud, 8N1 |
| **GND** | `GND` | `GND` | **Mandatory** common ground |

---

## 4. Building & Flashing

### A. ESP32-S3 Cloud Firmware (ESP-IDF v6.0.2)

1. Open terminal with ESP-IDF environment initialized:
   ```bash
   cd esp32_cloud
   idf.py set-target esp32s3
   idf.py build
   ```
2. Flash and monitor output:
   ```bash
   idf.py -p COM_PORT flash monitor
   ```

### B. nRF52840 Central Firmware (Zephyr RTOS / West)

1. Build using West:
   ```bash
   cd nrf_central
   west build -b nrf52840dk/nrf52840
   ```
2. Flash to board (via J-Link / OpenOCD):
   ```bash
   west flash
   ```

---

## 5. Host-Based Unit Testing (Zero-Hardware Verification)

All gateway logic (framing, JSON synthesis, session isolation, Wi-Fi backoff, MQTT outbox, handshake) can be tested instantly on any PC with GCC:

```bash
bash tools/run_tests.sh
```

**Output:**
```
==============================================
    RUNNING GATEWAY HOST UNIT TESTS (GCC)     
==============================================
  [PASS] test_data_synthesizer
  [PASS] test_gateway_mqtt_integration
  [PASS] test_uart_frame
  [PASS] test_wifi_portal_parser
  [PASS] test_wifi_backoff
  [PASS] test_bridge_handshake
  [PASS] test_gateway_ingest
  [PASS] test_softap_timeout
  [PASS] test_ble_connection_watchdog
  [PASS] test_system_health
  [PASS] test_gateway_time
  [PASS] test_uart_rx_parser
  [PASS] test_uart_tx_queue
  [PASS] test_gateway_mqtt_outbox
  [PASS] test_gateway_subsystems_init
  [PASS] test_ble_node_slots
  [PASS] test_provisioning_fsm
  [PASS] test_frame_snapshot
  [PASS] test_bridge_downlink
  [PASS] test_wifi_sta_config
  [PASS] test_gateway_downlink_pipeline
  [PASS] test_boot_button_logic
  [PASS] test_core1_gate
  [PASS] test_downlink_synthesizer
  [PASS] test_mqtt_lifecycle
  [PASS] test_ble_central_downlink
  [PASS] test_gateway_pipeline
  [PASS] test_bridge_time
==============================================
TOTAL: 28 | PASSED: 28 | FAILED: 0
==============================================
  🎉 ALL 28 GATEWAY HOST TESTS PASSED (100%)
```

---

## 6. License
Internal Healthcare IoT Prototype — All rights reserved.
