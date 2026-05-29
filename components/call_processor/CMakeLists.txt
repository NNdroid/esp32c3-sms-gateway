idf_component_register(
    SRCS "call_processor.c"
    INCLUDE_DIRS "include"
    REQUIRES modem_driver log_service push_service
)
