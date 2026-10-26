#!/usr/bin/env bash
# tools/run_tests.sh
# Clean out-of-source test runner for Gateway host unit tests
set -e

BUILD_DIR="_build/host"
mkdir -p "$BUILD_DIR"

PASS_COUNT=0
FAIL_COUNT=0

echo "=============================================="
echo "    RUNNING GATEWAY HOST UNIT TESTS (GCC)     "
echo "=============================================="

declare -A TEST_INCLUDES=(
    ["test_uart_frame"]="-I. -Icommon"
    ["test_ble_node_slots"]="-I. -Icommon -Inrf_central/src -Itests/mocks"
    ["test_uart_tx_queue"]="-I. -Icommon -Inrf_central/src -Itests/mocks"
    ["test_uart_rx_parser"]="-I. -Icommon -Iesp32_cloud/components/uart_bridge/include -Itests/mocks"
    ["test_data_synthesizer"]="-I. -Icommon -Iesp32_cloud/components/thingsboard_client/include -Iesp32_cloud/components/uart_bridge/include"
    ["test_wifi_backoff"]="-I. -Iesp32_cloud/components/wifi_manager/include"
    ["test_boot_button_logic"]="-I. -Iesp32_cloud/components/wifi_manager/include"
    ["test_softap_timeout"]="-I. -Iesp32_cloud/components/wifi_manager/include"
    ["test_wifi_portal_parser"]="-I. -Iesp32_cloud/components/wifi_manager/include"
    ["test_wifi_sta_config"]="-I. -Iesp32_cloud/components/wifi_manager/include"
    ["test_bridge_handshake"]="-I. -Icommon -Iesp32_cloud/components/uart_bridge/include -Inrf_central/src -Itests/mocks"
    ["test_core1_gate"]="-I. -Icommon -Iesp32_cloud/components/uart_bridge/include"
    ["test_frame_snapshot"]="-I. -Icommon -Iesp32_cloud/components/uart_bridge/include"
    ["test_bridge_time"]="-I. -Icommon"
    ["test_mqtt_lifecycle"]="-I. -Icommon -Iesp32_cloud/components/thingsboard_client/include -Itests/mocks"
    ["test_gateway_mqtt_integration"]="-I. -Icommon -Iesp32_cloud/components/thingsboard_client/include -Iesp32_cloud/components/uart_bridge/include"
    ["test_gateway_ingest"]="-I. -Icommon -Iesp32_cloud/components/uart_bridge/include"
    ["test_gateway_time"]="-I. -Icommon -Iesp32_cloud/components/uart_bridge/include"
    ["test_provisioning_fsm"]="-I. -Iesp32_cloud/components/wifi_manager/include"
    ["test_ble_connection_watchdog"]="-I. -Icommon -Inrf_central/src -Itests/mocks"
    ["test_gateway_mqtt_outbox"]="-I. -Itests/mocks -Iesp32_cloud/components/thingsboard_client/include"
    ["test_system_health"]="-I. -Itests/mocks -Icommon -Iesp32_cloud/main -Iesp32_cloud/components/uart_bridge/include -Iesp32_cloud/components/thingsboard_client/include"
    ["test_gateway_pipeline"]="-I. -Itests/mocks -Icommon -Iesp32_cloud/components/uart_bridge/include -Iesp32_cloud/components/thingsboard_client/include -Inrf_central/src"
    ["test_ble_central_downlink"]="-I. -Icommon -Inrf_central/src -Itests/mocks"
    ["test_bridge_downlink"]="-I. -Icommon -Inrf_central/src -Itests/mocks"
    ["test_downlink_synthesizer"]="-I. -Icommon -Iesp32_cloud/components/thingsboard_client/include"
    ["test_gateway_downlink_pipeline"]="-I. -Icommon -Iesp32_cloud/components/thingsboard_client/include -Iesp32_cloud/components/uart_bridge/include -Itests/mocks"
    ["test_gateway_subsystems_init"]="-I. -Itests/mocks -Icommon -Iesp32_cloud/main -Iesp32_cloud/components/uart_bridge/include -Iesp32_cloud/components/thingsboard_client/include -Iesp32_cloud/components/wifi_manager/include"
)

declare -A TEST_SOURCES=(
    ["test_uart_frame"]=""
    ["test_ble_node_slots"]="nrf_central/src/ble_node_slots.c"
    ["test_ble_connection_watchdog"]="nrf_central/src/ble_central_mgr.c nrf_central/src/ble_node_slots.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_uart_tx_queue"]="nrf_central/src/uart_tx_queue.c -DHOST_TEST"
    ["test_uart_rx_parser"]="esp32_cloud/components/uart_bridge/src/uart_rx_parser.c"
    ["test_data_synthesizer"]="esp32_cloud/components/thingsboard_client/src/data_synthesizer.c"
    ["test_wifi_backoff"]=""
    ["test_boot_button_logic"]=""
    ["test_softap_timeout"]=""
    ["test_wifi_portal_parser"]=""
    ["test_wifi_sta_config"]=""
    ["test_bridge_handshake"]="esp32_cloud/components/uart_bridge/src/uart_bridge_rx.c nrf_central/src/uart_tx_bridge.c esp32_cloud/components/uart_bridge/src/uart_rx_parser.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_core1_gate"]="esp32_cloud/components/uart_bridge/src/core1_gate.c"
    ["test_frame_snapshot"]="esp32_cloud/components/uart_bridge/src/frame_snapshot.c"
    ["test_bridge_time"]=""
    ["test_mqtt_lifecycle"]="esp32_cloud/components/thingsboard_client/src/thingsboard_mqtt.c -DHOST_UNIT_TEST"
    ["test_gateway_mqtt_integration"]="esp32_cloud/components/thingsboard_client/src/cloud_uplink.c esp32_cloud/components/thingsboard_client/src/thingsboard_mqtt.c esp32_cloud/components/thingsboard_client/src/data_synthesizer.c esp32_cloud/components/thingsboard_client/src/downlink_synthesizer.c esp32_cloud/components/uart_bridge/src/uart_bridge_rx.c -DHOST_UNIT_TEST"
    ["test_gateway_ingest"]="esp32_cloud/components/uart_bridge/src/gateway_ingest.c esp32_cloud/components/uart_bridge/src/gateway_time.c esp32_cloud/components/uart_bridge/src/uart_bridge_rx.c esp32_cloud/components/uart_bridge/src/core1_gate.c esp32_cloud/components/uart_bridge/src/frame_snapshot.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_gateway_time"]="esp32_cloud/components/uart_bridge/src/gateway_time.c -DHOST_UNIT_TEST"
    ["test_provisioning_fsm"]="esp32_cloud/components/wifi_manager/src/provisioning_fsm.c -DHOST_UNIT_TEST"
    ["test_gateway_mqtt_outbox"]="esp32_cloud/components/thingsboard_client/src/gateway_mqtt_outbox.c -DHOST_UNIT_TEST"
    ["test_system_health"]="esp32_cloud/main/system_health.c esp32_cloud/components/uart_bridge/src/frame_snapshot.c esp32_cloud/components/thingsboard_client/src/gateway_mqtt_outbox.c -DHOST_UNIT_TEST"
    ["test_gateway_pipeline"]="esp32_cloud/components/uart_bridge/src/gateway_ingest.c esp32_cloud/components/uart_bridge/src/gateway_time.c esp32_cloud/components/uart_bridge/src/uart_bridge_rx.c esp32_cloud/components/uart_bridge/src/core1_gate.c esp32_cloud/components/uart_bridge/src/frame_snapshot.c esp32_cloud/components/uart_bridge/src/uart_rx_parser.c esp32_cloud/components/thingsboard_client/src/cloud_uplink.c esp32_cloud/components/thingsboard_client/src/thingsboard_mqtt.c esp32_cloud/components/thingsboard_client/src/data_synthesizer.c esp32_cloud/components/thingsboard_client/src/downlink_synthesizer.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_ble_central_downlink"]="nrf_central/src/ble_central_mgr.c nrf_central/src/ble_node_slots.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_bridge_downlink"]="nrf_central/src/uart_tx_bridge.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_downlink_synthesizer"]="esp32_cloud/components/thingsboard_client/src/downlink_synthesizer.c"
    ["test_gateway_downlink_pipeline"]="esp32_cloud/components/thingsboard_client/src/downlink_synthesizer.c esp32_cloud/components/thingsboard_client/src/cloud_uplink.c esp32_cloud/components/thingsboard_client/src/thingsboard_mqtt.c esp32_cloud/components/thingsboard_client/src/data_synthesizer.c nrf_central/src/uart_tx_bridge.c -DHOST_UNIT_TEST -DHOST_TEST"
    ["test_gateway_subsystems_init"]="esp32_cloud/components/uart_bridge/src/uart_bridge.c esp32_cloud/components/uart_bridge/src/gateway_time.c esp32_cloud/components/uart_bridge/src/frame_snapshot.c esp32_cloud/components/uart_bridge/src/gateway_ingest.c esp32_cloud/components/uart_bridge/src/uart_bridge_rx.c esp32_cloud/components/uart_bridge/src/core1_gate.c esp32_cloud/components/uart_bridge/src/uart_rx_parser.c esp32_cloud/components/thingsboard_client/src/cloud_uplink.c esp32_cloud/components/thingsboard_client/src/thingsboard_mqtt.c esp32_cloud/components/thingsboard_client/src/data_synthesizer.c esp32_cloud/components/thingsboard_client/src/downlink_synthesizer.c -DHOST_UNIT_TEST -DHOST_TEST"
)

for test_name in "${!TEST_INCLUDES[@]}"; do
    test_src="tests/${test_name}.c"
    extra_src="${TEST_SOURCES[$test_name]}"
    inc="${TEST_INCLUDES[$test_name]}"
    out="${BUILD_DIR}/${test_name}.exe"

    if [ -f "$test_src" ]; then
        if gcc -Wall -Wextra $inc $test_src $extra_src -o "$out" 2>/dev/null; then
            if "$out" >/dev/null 2>&1; then
                echo "  [PASS] $test_name"
                PASS_COUNT=$((PASS_COUNT + 1))
            else
                echo "  [FAIL] $test_name (runtime error)"
                FAIL_COUNT=$((FAIL_COUNT + 1))
            fi
        else
            echo "  [FAIL] $test_name (compilation error)"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    else
        echo "  [MISSING] $test_src"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo "=============================================="
echo "TOTAL: $((PASS_COUNT + FAIL_COUNT)) | PASSED: $PASS_COUNT | FAILED: $FAIL_COUNT"
echo "=============================================="

if [ $FAIL_COUNT -eq 0 ]; then
    echo "  ALL $PASS_COUNT GATEWAY HOST TESTS PASSED (100%)"
    exit 0
else
    echo "  SOME TESTS FAILED"
    exit 1
fi
