idf_component_register(
    SRCS "src/thingsboard_mqtt.c" "src/tb_rest_client.c" "src/data_synthesizer.c"
    INCLUDE_DIRS "include"
    REQUIRES common esp_common freertos uart_bridge wifi_manager esp_http_client mbedtls esp_event esp_timer log
)
