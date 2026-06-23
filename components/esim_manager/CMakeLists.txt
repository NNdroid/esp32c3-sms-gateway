idf_component_register(
    SRCS "esim_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES modem_driver log_service config_manager
)
