#ifndef LOG_SERVICE_H
#define LOG_SERVICE_H

#include "sdkconfig.h"
#include <stdbool.h>
#include <stdarg.h>
#include "esp_log.h"

void log_service_init(void);
void log_service_set_ready(bool ready);
void log_service_voutput(int level, const char* tag, const char* format, va_list ap);

#ifdef ESP_LOGE
#undef ESP_LOGE
#endif
#ifdef ESP_LOGW
#undef ESP_LOGW
#endif
#ifdef ESP_LOGI
#undef ESP_LOGI
#endif
#ifdef ESP_LOGD
#undef ESP_LOGD
#endif
#ifdef ESP_LOGV
#undef ESP_LOGV
#endif

static inline void log_service_output(int level, const char* tag, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    log_service_voutput(level, tag, format, ap);
    va_end(ap);
}

#define ESP_LOGE(tag, format, ...) do { log_service_output(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, format, ...) do { log_service_output(ESP_LOG_WARN, tag, format, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, format, ...) do { log_service_output(ESP_LOG_INFO, tag, format, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, format, ...) do { log_service_output(ESP_LOG_DEBUG, tag, format, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, format, ...) do { log_service_output(ESP_LOG_VERBOSE, tag, format, ##__VA_ARGS__); } while (0)

#endif // LOG_SERVICE_H
