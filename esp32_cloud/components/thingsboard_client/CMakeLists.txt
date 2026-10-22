idf_component_register(
    SRCS
        "src/thingsboard_client.c"
        "src/thingsboard_mqtt.c"
        "src/data_synthesizer.c"
        "src/cloud_uplink.c"
        "src/downlink_synthesizer.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        mqtt
        uart_bridge
        common
        esp_timer
        freertos
)

if(CONFIG_MQTT_CUSTOM_OUTBOX)
    target_sources(mqtt_outbox_lib PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/gateway_mqtt_outbox.c")
    target_include_directories(mqtt_outbox_lib PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(${COMPONENT_LIB} PUBLIC idf::mqtt::outbox)
else()
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/gateway_mqtt_outbox.c")
endif()
