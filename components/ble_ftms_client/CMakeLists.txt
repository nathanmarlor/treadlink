idf_component_register(
    SRCS "ble_ftms_client.c"
    INCLUDE_DIRS "include"
    REQUIRES bt data_bridge config_store
)
