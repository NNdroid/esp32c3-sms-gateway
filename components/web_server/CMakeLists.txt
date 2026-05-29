idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server config_manager modem_driver sms_processor log esp_wifi app_update
    EMBED_FILES "web_assets/login.html"
                "web_assets/index.html"
                "web_assets/tools.html"
                "web_assets/ota.html"
)