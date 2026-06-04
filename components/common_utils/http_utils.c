#include "http_utils.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"

static const char *TAG = "HTTP_UTIL";

#define MAX_HTTP_RETRIES 3
#define BASE_RETRY_DELAY_MS 2000

// ⚠️ 治本神器：拦截 HTTP 底层狂发事件，解决大网页数据瞬间撑爆队列导致的 ESP_ERR_TIMEOUT 报错
static esp_err_t _http_event_handle(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

bool http_util_request(const char *url, esp_http_client_method_t method, const char *payload, const char *content_type)
{
    if (!url || strlen(url) == 0)
        return false;
    
    bool success = false;
    int delay_ms = BASE_RETRY_DELAY_MS;
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // 如果年份还是 1970 年，说明系统时间未同步，强行去验证证书 100% 会报 -0x7780
    int retry_time = 0;
    while (timeinfo.tm_year < (2024 - 1900) && retry_time < 10)
    {
        ESP_LOGW(TAG, "等待系统时间同步以便进行 HTTPS 握手... (%d/10)", retry_time + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
        retry_time++;
    }

    for (int attempt = 0; attempt <= MAX_HTTP_RETRIES; attempt++)
    {
        if (attempt > 0)
        {
            ESP_LOGW(TAG, "⚠️ 请求失败，等待 %d ms 后进行第 %d 次重试...", delay_ms, attempt);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            delay_ms *= 2; // 指数退避算法，防止频繁重发被服务器封杀 IP
        }

        // 每次发请求都全新初始化一个干净的 client，彻底解决跨域名复用导致的 SSL SNI 报错
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 15000,
            .buffer_size = 1024,
            .use_global_ca_store = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = _http_event_handle, // 挂载空回调，消除红色超时报错
        };

        //heap_caps_print_heap_info(MALLOC_CAP_8BIT);
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            ESP_LOGE(TAG, "❌ HTTP Client 初始化失败 (堆内存严重不足!)");
            continue; 
        }

        esp_http_client_set_method(client, method);

        // 如果是 POST 请求，挂载 Payload 数据
        if (method == HTTP_METHOD_POST && payload && content_type)
        {
            esp_http_client_set_header(client, "Content-Type", content_type);
            esp_http_client_set_post_field(client, payload, strlen(payload));
        }

        ESP_LOGD(TAG, ">>> 准备发送请求，当前剩余堆内存: %d 字节", (int)esp_get_free_heap_size());
        
        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code >= 200 && status_code < 300)
            {
                ESP_LOGI(TAG, "✅ HTTP [%d] 成功 | 目标: %.80s...", status_code, url);
                success = true;
            }
            else
            {
                ESP_LOGE(TAG, "❌ HTTP [%d] 异常 | 目标: %.80s...", status_code, url);
            }
            int content_length = esp_http_client_get_content_length(client);
            if (content_length > 0)
            {
                int read_len = content_length;
                if (read_len > 2048) {
                    read_len = 2048;
                }
                char buffer[2049] = {0};
                int received = esp_http_client_read(client, buffer, read_len);
                if (received >= 0)
                {
                    buffer[received] = '\0'; // 确保字符串结尾
                    ESP_LOGD(TAG, "<<< 响应内容: %.200s...", buffer);
                }
                else
                {
                    ESP_LOGW(TAG, "⚠️ 无法读取响应内容");
                }
            }
        }
        else
        {
            ESP_LOGE(TAG, "❌ 底层 TLS/TCP 错误: %s", esp_err_to_name(err));
        }

        // 无论成功失败，用完立刻清理释放内存
        esp_http_client_cleanup(client);

        if (success)
            break; // 成功则跳出重试循环
    }
    return success;
}