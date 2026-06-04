idf_component_register(SRCS "http_utils.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client mbedtls)