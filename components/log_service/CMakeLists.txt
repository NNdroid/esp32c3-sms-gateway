idf_component_register(
    SRCS "log_service.c"
    INCLUDE_DIRS "include"
    REQUIRES log config_manager esp_netif
)
