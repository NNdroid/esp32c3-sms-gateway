idf_component_register(
    SRCS "modem_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio esp_event freertos log_service
)