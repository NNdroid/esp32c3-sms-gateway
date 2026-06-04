idf_component_register(
    SRCS "cron_tasker.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos config_manager modem_driver cron_tasker log_service sms_processor
)