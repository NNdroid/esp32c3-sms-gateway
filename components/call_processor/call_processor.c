#include "call_processor.h"
#include "modem_driver.h"
#include "push_service.h"
#include "config_manager.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CALL_PROC";

static void handle_call_ringing(void* handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    (void) handler_arg;
    if (event_base != MODEM_EVENT || event_id != MODEM_EVENT_CALL_RINGING) {
        return;
    }

    const char *payload = (const char *)event_data;
    if (!payload) {
        ESP_LOGW(TAG, "收到空呼叫 URC");
        return;
    }

    // 解析来电号码
    const char *start = strchr(payload, '"');
    const char *end = NULL;
    if (start) {
        end = strchr(start + 1, '"');
    }

    char caller[32] = {0};
    if (start && end && end > start + 1) {
        size_t len = end - start - 1;
        if (len >= sizeof(caller)) len = sizeof(caller) - 1;
        memcpy(caller, start + 1, len);
        caller[len] = '\0';
    } else {
        strncpy(caller, payload, sizeof(caller) - 1);
        caller[sizeof(caller) - 1] = '\0';
    }

    ESP_LOGI(TAG, "来电响铃: %s", caller);

    char timestamp[64] = {0};
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);

    char message[128] = {0};
    snprintf(message, sizeof(message), "来电响铃，号码: %s", caller[0] ? caller : "未知");

    if (!g_app_config.callNotifyEnabled) {
        ESP_LOGI(TAG, "來電通知已禁用，忽略來電響鈴推送");
        return;
    }

    // 直接使用异步推送队列，不再為每次來電创建临时任务
    push_service_send(caller, message, timestamp);

    // 这里可以扩展为：
    // 1. 通知 Web UI
    // 2. 记录来电状态
}

void call_processor_init(void) {
    esp_event_handler_instance_register(MODEM_EVENT, MODEM_EVENT_CALL_RINGING, (esp_event_handler_t)handle_call_ringing, NULL, NULL);
    ESP_LOGI(TAG, "call_processor 已初始化，正在监听 MODEM_EVENT_CALL_RINGING");
}