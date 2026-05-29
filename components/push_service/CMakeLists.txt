idf_component_register(
    SRCS "push_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls config_manager esp_wifi modem_driver log_service
)
