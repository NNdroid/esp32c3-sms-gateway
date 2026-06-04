#include "push_service.h"
#include "config_manager.h"
#include "modem_driver.h"
#include "esp_http_client.h"
#include "http_utils.h"
#include "esp_log.h"
#include "log_service.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "PUSH_SVC";

#define PUSH_QUEUE_SIZE 10
#define CACHE_DURATION_MS (3 * 3600 * 1000)

#define URL_BUF_SIZE 1536
#define PAYLOAD_BUF_SIZE 1536

extern EventGroupHandle_t s_net_event_group;

typedef struct
{
    char value[64];
    uint32_t last_update;
} net_cache_item_t;

static net_cache_item_t cache_isp = {{0}, 0};
static net_cache_item_t cache_apn = {{0}, 0};
static net_cache_item_t cache_phone = {{0}, 0};

typedef struct
{
    char sender[64];
    char message[512];
    char timestamp[32];
    int sms_indexes[10];
    int sms_index_count;
} push_msg_t;

// 工作缓存结构体，调整大小后约占用 4.3 KB
typedef struct
{
    char target_url[URL_BUF_SIZE];
    char post_data[PAYLOAD_BUF_SIZE];
    char esc_sender[128];
    char esc_message[1024];
    char esc_time[64];
} push_buffers_t;

static QueueHandle_t s_push_queue = NULL;
static push_buffers_t s_push_bufs = {0};
static char s_push_url_sender[128];
static char s_push_url_time[128];
static char s_push_final_url[URL_BUF_SIZE];
static char s_push_raw_title[256];
static char s_push_url_title[256];
static char s_push_temp_payload[PAYLOAD_BUF_SIZE];

static bool is_cache_expired(net_cache_item_t *item, uint32_t now_ms)
{
    return (
        item->value[0] == '\0' ||
        strcmp(item->value, "Unknown") == 0 ||
        (now_ms - item->last_update > CACHE_DURATION_MS));
}

static void get_network_info_cached(char *apn, char *isp, char *phone)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    char resp_buf[256] = {0};

    if (is_cache_expired(&cache_isp, now))
    {
        if (modem_send_at_command("AT+COPS?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK)
        {
            char *p = strstr(resp_buf, ",\"");
            if (p)
            {
                p += 2;
                char *e = strchr(p, '"');
                if (e)
                {
                    int len = (e - p < 64) ? (e - p) : 63;
                    strncpy(cache_isp.value, p, len);
                    cache_isp.value[len] = '\0';
                    cache_isp.last_update = now;
                }
            }
        }
        if (cache_isp.value[0] == '\0')
            strcpy(cache_isp.value, "Unknown");
    }

    if (is_cache_expired(&cache_apn, now))
    {
        if (modem_send_at_command("AT+CGDCONT?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK)
        {
            char *p = strstr(resp_buf, ",\"");
            if (p)
            {
                p = strstr(p + 2, ",\"");
                if (p)
                {
                    p += 2;
                    char *e = strchr(p, '"');
                    if (e)
                    {
                        int len = (e - p < 64) ? (e - p) : 63;
                        strncpy(cache_apn.value, p, len);
                        cache_apn.value[len] = '\0';
                        cache_apn.last_update = now;
                    }
                }
            }
        }
        if (cache_apn.value[0] == '\0')
            strcpy(cache_apn.value, "Unknown");
    }

    if (is_cache_expired(&cache_phone, now))
    {
        if (modem_send_at_command("AT+CNUM", resp_buf, sizeof(resp_buf), 2000) == ESP_OK)
        {
            char *p = strstr(resp_buf, ",\"");
            if (p)
            {
                p += 2;
                char *e = strchr(p, '"');
                if (e)
                {
                    int len = (e - p < 64) ? (e - p) : 63;
                    strncpy(cache_phone.value, p, len);
                    cache_phone.value[len] = '\0';
                    cache_phone.last_update = now;
                }
            }
        }
        if (cache_phone.value[0] == '\0')
            strcpy(cache_phone.value, "Unknown");
    }

    strcpy(apn, cache_apn.value);
    strcpy(isp, cache_isp.value);
    strcpy(phone, cache_phone.value);
}

static void url_encode(const char *src, char *dest, size_t dest_len)
{
    const char *hex = "0123456789ABCDEF";
    size_t pos = 0;
    for (int i = 0; src[i] != '\0' && pos < dest_len - 3; i++)
    {
        unsigned char c = src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            dest[pos++] = c;
        }
        else if (c == ' ')
        {
            dest[pos++] = '+';
        }
        else
        {
            dest[pos++] = '%';
            dest[pos++] = hex[c >> 4];
            dest[pos++] = hex[c & 0x0F];
        }
    }
    dest[pos] = '\0';
}

static void json_escape(const char *src, char *dest, size_t dest_len)
{
    size_t pos = 0;
    for (int i = 0; src[i] != '\0' && pos < dest_len - 2; i++)
    {
        char c = src[i];
        if (c == '"')
        {
            dest[pos++] = '\\';
            dest[pos++] = '"';
        }
        else if (c == '\\')
        {
            dest[pos++] = '\\';
            dest[pos++] = '\\';
        }
        else if (c == '\n')
        {
            dest[pos++] = '\\';
            dest[pos++] = 'n';
        }
        else if (c == '\r')
        {
            dest[pos++] = '\\';
            dest[pos++] = 'r';
        }
        else if (c == '\t')
        {
            dest[pos++] = '\\';
            dest[pos++] = 't';
        }
        else
        {
            dest[pos++] = c;
        }
    }
    dest[pos] = '\0';
}

static void str_replace(char *target, const char *needle, const char *replacement, size_t max_len)
{
    if (!target || !needle || !replacement || max_len == 0 || max_len > 1536)
        return;
    char buffer[1536] = {0};

    char *insert_point = &buffer[0];
    const char *tmp = target;
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);

    while (1)
    {
        const char *p = strstr(tmp, needle);
        if (p == NULL)
        {
            if ((insert_point - buffer) + strlen(tmp) < max_len)
            {
                strcpy(insert_point, tmp);
            }
            break;
        }
        if ((insert_point - buffer) + (p - tmp) + repl_len >= max_len)
            break;

        memcpy(insert_point, tmp, p - tmp);
        insert_point += p - tmp;
        memcpy(insert_point, replacement, repl_len);
        insert_point += repl_len;
        tmp = p + needle_len;
    }
    size_t copy_len = strlen(buffer);
    if (copy_len >= max_len) {
        copy_len = max_len - 1;
    }
    memcpy(target, buffer, copy_len);
    target[copy_len] = '\0';
}

static bool send_to_single_channel(const push_channel_t *channel, const char *sender, const char *message, const char *timestamp, push_buffers_t *bufs)
{
    if (!channel || !channel->enabled || !bufs)
        return false;

    // 进场前清空工作区，防止上一次内容残留
    memset(bufs, 0, sizeof(push_buffers_t));
    bool is_ok = false;

    json_escape(sender, bufs->esc_sender, sizeof(bufs->esc_sender));
    json_escape(message, bufs->esc_message, sizeof(bufs->esc_message));
    json_escape(timestamp, bufs->esc_time, sizeof(bufs->esc_time));

    char ip_str[32] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        }
    }

    char mac_str[32] = "Unknown";
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    char apn_str[64] = "Unknown", isp_str[64] = "Unknown", phone_str[32] = "Unknown";
    get_network_info_cached(apn_str, isp_str, phone_str);

    char esc_apn[64], esc_isp[64], esc_ip[32], esc_mac[32], esc_phone[32];
    json_escape(apn_str, esc_apn, sizeof(esc_apn));
    json_escape(isp_str, esc_isp, sizeof(esc_isp));
    json_escape(phone_str, esc_phone, sizeof(esc_phone));
    json_escape(ip_str, esc_ip, sizeof(esc_ip));
    json_escape(mac_str, esc_mac, sizeof(esc_mac));

    strncpy(bufs->target_url, channel->url, URL_BUF_SIZE - 1);

    if (channel->type == PUSH_TYPE_SERVERCHAN && strlen(bufs->target_url) == 0)
    {
        snprintf(bufs->target_url, URL_BUF_SIZE, "https://sctapi.ftqq.com/%s.send", channel->key1);
    }
    else if (channel->type == PUSH_TYPE_TELEGRAM && strlen(bufs->target_url) == 0)
    {
        snprintf(bufs->target_url, URL_BUF_SIZE, "https://api.telegram.org/bot%s/sendMessage", channel->key2);
    }

    esp_http_client_method_t method = HTTP_METHOD_POST;
    const char *content_type = "application/json";

    switch (channel->type)
    {
    case PUSH_TYPE_POST_JSON:
        snprintf(bufs->post_data, PAYLOAD_BUF_SIZE, "{\"sender\":\"%.100s\",\"message\":\"%.1000s\",\"timestamp\":\"%.50s\"}", bufs->esc_sender, bufs->esc_message, bufs->esc_time);
        break;

    case PUSH_TYPE_BARK:
        snprintf(bufs->post_data, PAYLOAD_BUF_SIZE, "{\"title\":\"%.100s\",\"body\":\"%.1000s\"}", bufs->esc_sender, bufs->esc_message);
        break;

    case PUSH_TYPE_GET:
    {
        if (strlen(message) > 2000)
        {
            ESP_LOGE(TAG, "GET URL 请求过长，超出上限");
            return false;
        }
        method = HTTP_METHOD_GET;
        url_encode(sender, s_push_url_sender, sizeof(s_push_url_sender));
        url_encode(timestamp, s_push_url_time, sizeof(s_push_url_time));

        url_encode(message, bufs->post_data, PAYLOAD_BUF_SIZE);

        const char *separator = (strchr(bufs->target_url, '?') == NULL) ? "?" : "&";

        size_t remaining = sizeof(s_push_final_url);
        char *p = s_push_final_url;
        int written = snprintf(p, remaining, "%s%s", bufs->target_url, separator);
        if (written < 0) {
            s_push_final_url[0] = '\0';
        } else {
            if ((size_t)written >= remaining) {
                p = s_push_final_url + sizeof(s_push_final_url) - 1;
                remaining = 1;
                *p = '\0';
            } else {
                p += written;
                remaining -= written;
            }

            written = snprintf(p, remaining, "sender=%.*s", (int)(remaining > 1 ? remaining - 1 : 0), s_push_url_sender);
            if (written < 0) {
                *p = '\0';
            } else if ((size_t)written >= remaining) {
                p = s_push_final_url + sizeof(s_push_final_url) - 1;
                remaining = 1;
                *p = '\0';
            } else {
                p += written;
                remaining -= written;
            }

            written = snprintf(p, remaining, "&message=%.*s", (int)(remaining > 1 ? remaining - 1 : 0), bufs->post_data);
            if (written < 0) {
                *p = '\0';
            } else if ((size_t)written >= remaining) {
                p = s_push_final_url + sizeof(s_push_final_url) - 1;
                remaining = 1;
                *p = '\0';
            } else {
                p += written;
                remaining -= written;
            }

            written = snprintf(p, remaining, "&timestamp=%.*s", (int)(remaining > 1 ? remaining - 1 : 0), s_push_url_time);
            if (written < 0 || (size_t)written >= remaining) {
                if (remaining > 0) {
                    s_push_final_url[sizeof(s_push_final_url) - 1] = '\0';
                }
            }
        }

        snprintf(bufs->target_url, URL_BUF_SIZE, "%s", s_push_final_url);
        bufs->post_data[0] = '\0';
        break;
    }

    case PUSH_TYPE_SERVERCHAN:
    {
        content_type = "application/x-www-form-urlencoded";
        snprintf(s_push_raw_title, sizeof(s_push_raw_title), "短信来自:%.100s", sender);
        url_encode(s_push_raw_title, s_push_url_title, sizeof(s_push_url_title));

        snprintf(bufs->esc_message, 1024, "**时间:** %.50s\n\n**内容:**\n\n%.800s", timestamp, message);
        url_encode(bufs->esc_message, bufs->post_data, PAYLOAD_BUF_SIZE);

        snprintf(s_push_temp_payload, sizeof(s_push_temp_payload), "title=%.200s&desp=%.1200s", s_push_url_title, bufs->post_data);
        strcpy(bufs->post_data, s_push_temp_payload);
        break;
    }

    case PUSH_TYPE_GOTIFY:
    {
        snprintf(bufs->target_url, URL_BUF_SIZE, "%.1000s%smessage?token=%.100s",
                 channel->url, (channel->url[strlen(channel->url) - 1] == '/') ? "" : "/", channel->key1);

        snprintf(bufs->post_data, PAYLOAD_BUF_SIZE, "{\"title\":\"短信来自: %.100s\",\"message\":\"%.1000s\\n\\n时间: %.50s\",\"priority\":5}",
                 bufs->esc_sender, bufs->esc_message, bufs->esc_time);
        break;
    }

    case PUSH_TYPE_TELEGRAM:
        snprintf(bufs->post_data, PAYLOAD_BUF_SIZE, "{\"chat_id\":\"%.100s\",\"text\":\"📱通知\\n发送者: %.100s\\n内容: %.1000s\\n时间: %.50s\"}",
                 channel->key1, bufs->esc_sender, bufs->esc_message, bufs->esc_time);
        break;

    case PUSH_TYPE_CUSTOM:
        snprintf(bufs->post_data, PAYLOAD_BUF_SIZE, "%.1500s", channel->customBody);
        str_replace(bufs->post_data, "{sender}", bufs->esc_sender, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{message}", bufs->esc_message, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{timestamp}", bufs->esc_time, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{isp}", esc_isp, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{apn}", esc_apn, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{device}", esc_mac, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{ip}", esc_ip, PAYLOAD_BUF_SIZE);
        str_replace(bufs->post_data, "{phone}", esc_phone, PAYLOAD_BUF_SIZE);
        break;

    default:
        return false;
    }

    ESP_LOGI(TAG, "通道 [%s] 准备请求 URL: %s", channel->name, bufs->target_url);

    // 🚀 这里直接调用抽离出来的通用接口，一行代码搞定网络请求、重试、释放内存！
    is_ok = http_util_request(bufs->target_url, method, bufs->post_data, content_type);

    if (!is_ok)
        ESP_LOGE(TAG, "通道 [%s] 最终推送失败", channel->name);
    else
        ESP_LOGI(TAG, "通道 [%s] 推送成功！", channel->name);

    return is_ok;
}

static void push_service_task(void *pvParameters)
{
    push_msg_t msg;
    push_buffers_t *bufs = &s_push_bufs;

    while (1)
    {
        xEventGroupWaitBits(s_net_event_group, NET_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        time_t now;
        time(&now);

        if ((long long)now < 1704067200LL)
        {
            ESP_LOGW(TAG, "⏰ 时间尚未同步，等待校准后再推送...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (xQueueReceive(s_push_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGI(TAG, "🔄 开始后台推送: sender=%s", msg.sender);

            bool any_success = false;

            for (int i = 0; i < MAX_PUSH_CHANNELS; i++)
            {
                if (g_app_config.pushChannels[i].enabled)
                {
                    // 传入伴生内存 bufs
                    bool ok = send_to_single_channel(&g_app_config.pushChannels[i], msg.sender, msg.message, msg.timestamp, bufs);
                    if (ok)
                        any_success = true;
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }

            if (any_success && msg.sms_index_count > 0)
            {
                ESP_LOGI(TAG, "✅ 推送成功，清理底层短信: %d 条", msg.sms_index_count);
                for (int i = 0; i < msg.sms_index_count; i++)
                {
                    modem_delete_sms(msg.sms_indexes[i]);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            else if (!any_success && msg.sms_index_count > 0)
            {
                ESP_LOGW(TAG, "❌ 所有推送通道均失败，保留短信不删除");
            }
        }
    }
}

void push_service_init(void)
{
    if (s_push_queue == NULL)
    {
        s_push_queue = xQueueCreate(PUSH_QUEUE_SIZE, sizeof(push_msg_t));
        xTaskCreate(push_service_task, "push_task", 12288, NULL, 5, NULL);
        ESP_LOGI(TAG, "🚀 异步推送服务及后台队列初始化完成");
    }
}

void push_service_send(const char *sender, const char *message, const char *timestamp)
{
    if (s_push_queue == NULL)
        return;

    push_msg_t msg = {0};
    if (sender)
        strncpy(msg.sender, sender, sizeof(msg.sender) - 1);
    if (message)
        strncpy(msg.message, message, sizeof(msg.message) - 1);
    if (timestamp)
        strncpy(msg.timestamp, timestamp, sizeof(msg.timestamp) - 1);

    if (xQueueSend(s_push_queue, &msg, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "❌ 推送队列已满，丢弃消息");
    }
    else
    {
        ESP_LOGI(TAG, "📥 消息已压入队列");
    }
}

void push_service_send_now(const char *sender, const char *message)
{
    char timestamp[32];
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);

    push_service_send(sender, message, timestamp);
}

void push_service_send_with_ack(const char *sender, const char *message, const char *timestamp, const int *indexes, int index_count)
{
    if (s_push_queue == NULL)
        return;

    push_msg_t msg = {0};
    if (sender)
        strncpy(msg.sender, sender, sizeof(msg.sender) - 1);
    if (message)
        strncpy(msg.message, message, sizeof(msg.message) - 1);
    if (timestamp)
        strncpy(msg.timestamp, timestamp, sizeof(msg.timestamp) - 1);

    if (indexes != NULL && index_count > 0)
    {
        msg.sms_index_count = (index_count > 10) ? 10 : index_count;
        for (int i = 0; i < msg.sms_index_count; i++)
        {
            msg.sms_indexes[i] = indexes[i];
        }
    }

    if (xQueueSend(s_push_queue, &msg, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "❌ 推送队列已满，丢弃消息");
    }
}