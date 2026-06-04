set(REQUIRES_LIST config_manager modem_driver sms_processor cron_tasker web_server network_provisioning call_processor)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES_LIST}
)