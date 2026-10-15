idf_component_register(
    SRCS "src/wifi_sta_mgr.c" "src/wifi_ap.c" "src/wifi_portal.c" "src/boot_button.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos bsp_storage esp_wifi esp_event esp_netif esp_timer esp_http_server esp_driver_gpio lwip log
    PRIV_REQUIRES thingsboard_client
    EMBED_TXTFILES "index.html"
)
