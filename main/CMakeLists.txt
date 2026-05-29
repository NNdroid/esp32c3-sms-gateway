idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES config_manager modem_driver sms_processor call_processor cron_tasker web_server network_provisioning
)