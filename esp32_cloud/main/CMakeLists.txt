idf_component_register(
    SRCS 
        "main.c"
        "wifi_sta_mgr.c"
        "wifi_portal.c"
        "boot_button.c"
        "wifi_ap.c"
        "thingsboard_mqtt.c"
        "uart_bridge_rx.c"
        "uart_rx_parser.c"
        "data_synthesizer.c"
        "nvs_storage.c"
        "tb_rest_client.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES esp_http_server nvs_flash esp_wifi spi_flash esp_driver_uart esp_driver_gpio esp_http_client mbedtls
    EMBED_TXTFILES "index.html"
)