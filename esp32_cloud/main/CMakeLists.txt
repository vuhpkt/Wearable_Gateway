idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager thingsboard_client uart_bridge bsp_storage common esp_event esp_netif esp_timer esp_system freertos log
)