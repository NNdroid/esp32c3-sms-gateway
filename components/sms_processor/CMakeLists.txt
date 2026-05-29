idf_component_register(
    SRCS "sms_processor.c" "pdu_decoder.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log_service config_manager modem_driver push_service
)