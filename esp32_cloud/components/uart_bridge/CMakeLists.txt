idf_component_register(
    SRCS "src/uart_bridge_rx.c" "src/uart_rx_parser.c"
    INCLUDE_DIRS "include"
    REQUIRES common esp_common esp_driver_uart log
)
