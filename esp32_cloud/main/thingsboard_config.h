#pragma once

/* ══════════════════════════════════════════════════════════════
 *  TARGET CLOUD SWITCH FLAG
 *  1: Local Mock MQTT Broker (Chạy script gateway/tools/mock_server/server.py trên PC)
 *  0: ThingsBoard Server (Trích xuất từ Wearable_Server: 103.116.39.179 / mqtt.thingsboard.cloud)
 * ════════════════════════════════════════════════════════════ */
#define USE_MOCK_MQTT_SERVER            0

#if USE_MOCK_MQTT_SERVER
    /* ─── 1. LOCAL MOCK MQTT BROKER (Zero-Cloud, test nội bộ) ─── */
    #define TB_MQTT_BROKER_URI          "mqtt://192.168.2.3:1883"
    #define TB_GATEWAY_ACCESS_TOKEN     "GATEWAY_LOCAL_TEST_TOKEN"
#else
    /* ─── 2. THINGSBOARD MQTT SERVER (Trích xuất từ Wearable_Server) ───
     * - Server C7-2Slab (từ Wearable_Server/.env.example): "mqtt://103.116.39.179:1883"
     * - ThingsBoard Cloud công cộng (từ Wearable_Server/STREAMING_INSTRUCTION.md): "mqtt://mqtt.thingsboard.cloud:1883"
     * Điền Token của thiết bị Gateway tạo trên ThingsBoard vào TB_GATEWAY_ACCESS_TOKEN.
     */
    #define TB_MQTT_BROKER_URI          "mqtt://103.116.39.179:1883"
    #define TB_GATEWAY_ACCESS_TOKEN     "NPupkjwg0hoAPiPh4cTk"
#endif

/* ─── Topics chuẩn của ThingsBoard Gateway API ─── */
#define TB_GATEWAY_TELEMETRY_TOPIC      "v1/gateway/telemetry"
#define TB_GATEWAY_CONNECT_TOPIC        "v1/gateway/connect"
#define TB_GATEWAY_DISCONNECT_TOPIC     "v1/gateway/disconnect"
#define TB_GATEWAY_ATTRIBUTES_TOPIC     "v1/gateway/attributes"

/* ─── Cấu hình ThingsBoard REST API (Cập nhật SERVER_SCOPE cho Web Dashboard) ─── */
#define TB_REST_BASE_URL                "https://c7.hust-2slab.org"
#define TB_REST_USERNAME                "tenant@thingsboard.org"
#define TB_REST_PASSWORD                "tenant"
#define TB_NODE1_DEVICE_UUID            "20c35db0-c64a-11f1-a73c-47ac28e322d1"

/* Chu kỳ gom bản tin đẩy lên Cloud (Batching interval): 1000 ms (1 giây) */
#define TB_BATCH_INTERVAL_MS            1000U