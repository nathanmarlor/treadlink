idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ble_common ble_ftms_client ble_rsc_server data_bridge
             wifi_manager web_server led_status config_store
)
