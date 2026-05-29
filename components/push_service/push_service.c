#include "push_service.h"
#include "config_manager.h"
#include "modem_driver.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "log_service.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *TAG = "PUSH_SVC";

#define MAX_HTTP_RETRIES      3     // 最大重试次数（总共执行 1 + 3 = 4 次）
#define BASE_RETRY_DELAY_MS   2000  // 基础延迟时间 2 秒

// ================= 全局网络状态 & 本机号码缓存 =================
static char cached_apn[64] = "";
static char cached_isp[64] = "";
static char cached_phone[32] = "";
static uint32_t last_net_query_time = 0;

static void get_network_info_cached(char *apn, char *isp, char *phone) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // 如果缓存为空，或者距离上次查询超过了 6 小时 (21600000 ms)，才去查询底层模组
    if (cached_apn[0] == '\0' || (now - last_net_query_time > 21600000)) {
        ESP_LOGI(TAG, "🔄 正在更新底层网络参数 (APN/ISP/号码) 缓存...");
        char resp_buf[256] = {0};
        
        // 1. 查询 ISP (电信运营商)
        if (modem_send_at_command("AT+COPS?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, ",\"");
            if (p) {
                p += 2;
                char *e = strchr(p, '"');
                if (e) {
                    int len = e - p;
                    if (len >= sizeof(cached_isp)) len = sizeof(cached_isp) - 1;
                    strncpy(cached_isp, p, len);
                    cached_isp[len] = '\0';
                }
            }
        }
        
        // 2. 查询 APN (接入点名称)
        if (modem_send_at_command("AT+CGDCONT?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, ",\"");
            if (p) {
                p = strstr(p + 2, ",\"");
                if (p) {
                    p += 2;
                    char *e = strchr(p, '"');
                    if (e) {
                        int len = e - p;
                        if (len >= sizeof(cached_apn)) len = sizeof(cached_apn) - 1;
                        strncpy(cached_apn, p, len);
                        cached_apn[len] = '\0';
                    }
                }
            }
        }

        // 3. 🚀 查询 SIM 卡本机号码 (AT+CNUM)
        // 格式通常为: +CNUM: "MyNumber","+8613800138000",145
        if (modem_send_at_command("AT+CNUM", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, ",\"");
            if (p) {
                p += 2;
                char *e = strchr(p, '"');
                if (e) {
                    int len = e - p;
                    if (len >= sizeof(cached_phone)) len = sizeof(cached_phone) - 1;
                    strncpy(cached_phone, p, len);
                    cached_phone[len] = '\0';
                }
            }
        }
        
        if (cached_isp[0] == '\0') strcpy(cached_isp, "Unknown");
        if (cached_apn[0] == '\0') strcpy(cached_apn, "Unknown");
        if (cached_phone[0] == '\0') strcpy(cached_phone, "Unknown");
        
        last_net_query_time = now;
        ESP_LOGI(TAG, "✅ 缓存更新完毕 -> ISP: %s | APN: %s | 手机号: %s", cached_isp, cached_apn, cached_phone);
    }
    
    strcpy(apn, cached_apn);
    strcpy(isp, cached_isp);
    strcpy(phone, cached_phone);
}

// 简单的 URL 编码实现
static void url_encode(const char *src, char *dest, size_t dest_len) {
    const char *hex = "0123456789ABCDEF";
    size_t pos = 0;
    for (int i = 0; src[i] != '\0' && pos < dest_len - 3; i++) {
        unsigned char c = src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dest[pos++] = c;
        } else if (c == ' ') {
            dest[pos++] = '+';
        } else {
            dest[pos++] = '%';
            dest[pos++] = hex[c >> 4];
            dest[pos++] = hex[c & 0x0F];
        }
    }
    dest[pos] = '\0';
}

// 简单的 JSON 转义实现
static void json_escape(const char *src, char *dest, size_t dest_len) {
    size_t pos = 0;
    for (int i = 0; src[i] != '\0' && pos < dest_len - 2; i++) {
        char c = src[i];
        if (c == '"') { dest[pos++] = '\\'; dest[pos++] = '"'; }
        else if (c == '\\') { dest[pos++] = '\\'; dest[pos++] = '\\'; }
        else if (c == '\n') { dest[pos++] = '\\'; dest[pos++] = 'n'; }
        else if (c == '\r') { dest[pos++] = '\\'; dest[pos++] = 'r'; }
        else if (c == '\t') { dest[pos++] = '\\'; dest[pos++] = 't'; }
        else { dest[pos++] = c; }
    }
    dest[pos] = '\0';
}

// 安全的堆内存字符串替换 (防止炸栈)
static void str_replace(char *target, const char *needle, const char *replacement, size_t max_len) {
    if (!target || !needle || !replacement) return;
    char *buffer = calloc(1, max_len);
    if (!buffer) return;
    
    char *insert_point = &buffer[0];
    const char *tmp = target;
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);

    while (1) {
        const char *p = strstr(tmp, needle);
        if (p == NULL) {
            if (strlen(insert_point) + strlen(tmp) < max_len) {
                strcpy(insert_point, tmp);
            }
            break;
        }
        if ((insert_point - buffer) + (p - tmp) + repl_len >= max_len) break;
        
        memcpy(insert_point, tmp, p - tmp);
        insert_point += p - tmp;
        memcpy(insert_point, replacement, repl_len);
        insert_point += repl_len;
        tmp = p + needle_len;
    }
    strncpy(target, buffer, max_len - 1);
    free(buffer);
}

/**
 * @brief 带智能重试机制的 HTTP POST 执行器
 * * @param url 请求的目标地址
 * @param payload 组装好的 JSON 或 URL-Encoded 请求体
 * @param content_type "application/json" 或 "application/x-www-form-urlencoded"
 * @return true 推送成功
 * @return false 彻底失败
 */
static bool http_request_with_retry(const char* url, esp_http_client_method_t method, const char* payload, const char* content_type) {
    esp_err_t err;
    int status_code = 0;
    bool success = false;
    int delay_ms = BASE_RETRY_DELAY_MS;

    for (int attempt = 0; attempt <= MAX_HTTP_RETRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "⚠️ HTTP 推送失败，等待 %d ms 后进行第 %d 次重试...", delay_ms, attempt);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            delay_ms *= 2; 
        }

        esp_http_client_config_t config = {
            .url = url,
            .method = method,
            .timeout_ms = 10000, 
            .crt_bundle_attach = esp_crt_bundle_attach, 
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "HTTP Client 初始化失败 (内存不足)");
            continue;
        }

        if (method == HTTP_METHOD_POST && payload != NULL && content_type != NULL) {
            esp_http_client_set_header(client, "Content-Type", content_type);
            esp_http_client_set_post_field(client, payload, strlen(payload));
        }

        err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            status_code = esp_http_client_get_status_code(client);
            if (status_code >= 200 && status_code < 300) {
                ESP_LOGI(TAG, "✅ 推送成功! 状态码: %d", status_code);
                success = true;
                esp_http_client_cleanup(client);
                break;
            } else {
                ESP_LOGE(TAG, "❌ 推送失败，服务器拒绝，状态码: %d", status_code);
                if (status_code >= 400 && status_code < 500 && status_code != 408 && status_code != 429) {
                    ESP_LOGE(TAG, "🚨 客户端配置/授权错误，放弃重试。");
                    esp_http_client_cleanup(client);
                    break;
                }
            }
        } else {
            ESP_LOGE(TAG, "❌ 底层 TCP/TLS 通信失败: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }
    return success;
}

static void send_to_single_channel(const push_channel_t* channel, const char* sender, const char* message, const char* timestamp) {
    if (!channel || !channel->enabled) return;

    // 分配堆内存防止栈溢出
    char *target_url = calloc(1, 1024);
    char *post_data  = calloc(1, 2048);
    char *esc_sender = calloc(1, 128);
    char *esc_message= calloc(1, 1024);
    char *esc_time   = calloc(1, 128);

    if (!target_url || !post_data || !esc_sender || !esc_message || !esc_time) {
        ESP_LOGE(TAG, "内存分配失败，放弃推送");
        goto cleanup;
    }

    // 转义基础数据
    json_escape(sender, esc_sender, 128);
    json_escape(message, esc_message, 1024);
    json_escape(timestamp, esc_time, 128);

    // ================= 获取底层参数 (懒加载高速缓存) =================
    char ip_str[32] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        }
    }

    char mac_str[32] = "Unknown";
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    char apn_str[64] = "Unknown";
    char isp_str[64] = "Unknown";
    char phone_str[32] = "Unknown"; // 🚀 准备接收本机号码
    
    // 查询缓存 (0卡顿)
    get_network_info_cached(apn_str, isp_str, phone_str);

    // 转义扩充数据
    char esc_apn[64], esc_isp[64], esc_ip[32], esc_mac[32], esc_phone[32];
    json_escape(apn_str, esc_apn, sizeof(esc_apn));
    json_escape(isp_str, esc_isp, sizeof(esc_isp));
    json_escape(phone_str, esc_phone, sizeof(esc_phone)); // 🚀 转义手机号
    json_escape(ip_str, esc_ip, sizeof(esc_ip));
    json_escape(mac_str, esc_mac, sizeof(esc_mac));

    // =============================================================

    strncpy(target_url, channel->url, 1023);
    if (channel->type == PUSH_TYPE_SERVERCHAN && strlen(target_url) == 0) {
        snprintf(target_url, 1024, "https://sctapi.ftqq.com/%s.send", channel->key1);
    } else if (channel->type == PUSH_TYPE_TELEGRAM && strlen(target_url) == 0) {
        snprintf(target_url, 1024, "https://api.telegram.org/bot%s/sendMessage", channel->key2);
    }

    esp_http_client_method_t method = HTTP_METHOD_POST;
    const char *content_type = "application/json";

    switch (channel->type) {
        case PUSH_TYPE_POST_JSON:
            snprintf(post_data, 2048, "{\"sender\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%s\"}", esc_sender, esc_message, esc_time);
            break;

        case PUSH_TYPE_BARK:
            snprintf(post_data, 2048, "{\"title\":\"%s\",\"body\":\"%s\"}", esc_sender, esc_message);
            break;

        case PUSH_TYPE_GET: {
            method = HTTP_METHOD_GET;
            char *url_sender = calloc(1, 128);
            char *url_message= calloc(1, 1024);
            char *url_time   = calloc(1, 128);
            
            url_encode(sender, url_sender, 128);
            url_encode(message, url_message, 1024);
            url_encode(timestamp, url_time, 128);

            const char* separator = (strchr(target_url, '?') == NULL) ? "?" : "&";
            char *base_url = strdup(target_url);
            snprintf(target_url, 1024, "%s%ssender=%s&message=%s&timestamp=%s", base_url, separator, url_sender, url_message, url_time);
            
            free(base_url); free(url_sender); free(url_message); free(url_time);
            break;
        }

        case PUSH_TYPE_SERVERCHAN: {
            content_type = "application/x-www-form-urlencoded";
            char *url_title = calloc(1, 256);
            char *url_desp  = calloc(1, 2048);
            char *raw_desp  = calloc(1, 1024);
            
            snprintf(raw_desp, 1024, "**时间:** %s\n\n**内容:**\n\n%s", timestamp, message);
            url_encode(sender, url_title, 256);
            url_encode(raw_desp, url_desp, 2048);
            
            snprintf(post_data, 2048, "title=短信来自:%s&desp=%s", url_title, url_desp);
            free(url_title); free(url_desp); free(raw_desp);
            break;
        }

        case PUSH_TYPE_GOTIFY:
            snprintf(target_url, 1024, "%s%smessage?token=%s", channel->url, (channel->url[strlen(channel->url)-1] == '/') ? "" : "/", channel->key1);
            snprintf(post_data, 2048, "{\"title\":\"短信来自: %s\",\"message\":\"%s\\n\\n时间: %s\",\"priority\":5}", esc_sender, esc_message, esc_time);
            break;

        case PUSH_TYPE_TELEGRAM:
            snprintf(post_data, 2048, "{\"chat_id\":\"%s\",\"text\":\"📱通知\\n发送者: %s\\n内容: %s\\n时间: %s\"}", channel->key1, esc_sender, esc_message, esc_time);
            break;

        case PUSH_TYPE_CUSTOM:
            // 💡 动态模板字符替换
            strncpy(post_data, channel->customBody, 2047);
            str_replace(post_data, "{sender}", esc_sender, 2048);
            str_replace(post_data, "{message}", esc_message, 2048);
            str_replace(post_data, "{timestamp}", esc_time, 2048);
            str_replace(post_data, "{isp}", esc_isp, 2048);
            str_replace(post_data, "{apn}", esc_apn, 2048);
            str_replace(post_data, "{device}", esc_mac, 2048);
            str_replace(post_data, "{ip}", esc_ip, 2048);
            str_replace(post_data, "{phone}", esc_phone, 2048); // 🚀 注入新占位符
            break;

        default:
            ESP_LOGE(TAG, "未知的通道类型: %d", channel->type);
            goto cleanup;
    }

    ESP_LOGI(TAG, "通道 [%s] 准备请求 URL: %s", channel->name, target_url);

    // 执行 HTTP 请求
    bool is_ok = http_request_with_retry(target_url, method, post_data, content_type);
    if (!is_ok) {
        ESP_LOGE(TAG, "通道 [%s] 最终推送失败", channel->name);
    }

cleanup:
    // 统一清理堆内存
    if (target_url)  free(target_url);
    if (post_data)   free(post_data);
    if (esc_sender)  free(esc_sender);
    if (esc_message) free(esc_message);
    if (esc_time)    free(esc_time);
}

void push_service_init(void) {
    ESP_LOGI(TAG, "push_service 已初始化");
}

void push_service_send(const char* sender, const char* message, const char* timestamp) {
    ESP_LOGI(TAG, "开始推送通知: sender=%s", sender);

    // 遍历所有已启用的通道分别推送
    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        if (g_app_config.pushChannels[i].enabled) {
            send_to_single_channel(&g_app_config.pushChannels[i], sender, message, timestamp);
            vTaskDelay(pdMS_TO_TICKS(50)); 
        }
    }
}