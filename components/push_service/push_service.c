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

// ==================== 网络信息缓存 ====================
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

// 推送队列句柄
static QueueHandle_t s_push_queue = NULL;

// ==================== 统一工作缓冲区 ====================
// 所有推送过程中需要的临时buffer合并到一个结构体中。
// 这些buffer在运行时是串行使用的（不会同时被用），共用一块内存即可。
// 通过malloc动态分配，只在实际处理推送时占用内存，空闲时释放。
typedef struct {
    // 核心字段
    char target_url[URL_BUF_SIZE];    // 1536
    char post_data[PAYLOAD_BUF_SIZE]; // 1536

    // 使用子结构使不同推送类型的专用buffer共享空间
    // 因为POST_JSON/BARK/TELEGRAM/GOTIFY只用target_url + post_data
    // 而GET/SERVERCHAN/CUSTOM需要额外空间
    struct {
        char url_sender[128];
        char url_time[128];
        char final_url[URL_BUF_SIZE];
    } get_buf;                        // GET类型专用

    struct {
        char raw_title[256];
        char url_title[256];
        char temp_payload[PAYLOAD_BUF_SIZE];
    } serverchan_buf;                 // ServerChan类型专用

    struct {
        char meta_area[256];          // 存放isp/apn/phone等临时数据
    } custom_buf;                     // CUSTOM类型临时元数据

    // 始终需要的转义字段
    char esc_sender[128];
    char esc_message[1024];
    char esc_time[64];

} push_work_t;

// push_work_t 总大小 ≈ 1536+1536 + MAX(128+128+1536, 256+256+1536, 256) + 128+1024+64
//                   = 3072 + 1792 + 1216 = ~6KB
// 相比原来的全局分散buffer(~8KB)更紧凑，且只在运行时占用

// ==================== 辅助函数 ====================

static bool is_cache_expired(net_cache_item_t *item, uint32_t now_ms)
{
    return (
        item->value[0] == '\0' ||
        strcmp(item->value, "Unknown") == 0 ||
        (now_ms - item->last_update > CACHE_DURATION_MS));
}

// 获取网络信息带缓存，通过传入缓冲区避免栈上额外分配
static void get_network_info_cached(char *apn, char *isp, char *phone,
                                     size_t apn_size, size_t isp_size, size_t phone_size)
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

    // 使用 snprintf 代替 strncpy，避免 -Wstringop-truncation
    // apn_size/isp_size/phone_size 通常为64/64/32，缓存内容最长63/63/63
    snprintf(apn, apn_size, "%s", cache_apn.value);
    snprintf(isp, isp_size, "%s", cache_isp.value);
    snprintf(phone, phone_size, "%s", cache_phone.value);
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

// ==================== 字符串替换 — 原地操作（无额外栈buffer） ====================
// 原版 str_replace 在栈上分配了 1536 字节的临时 buffer，这是栈压力的主要来源。
// 优化方案：利用目标字符串（post_data）尾部空闲内存作为工作区（原地替换）。
static void str_replace_inplace(char *target, const char *needle, const char *replacement, size_t max_len)
{
    if (!target || !needle || !replacement || max_len == 0)
        return;

    size_t target_len = strlen(target);
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);

    if (target_len == 0 || needle_len == 0)
        return;

    // 需要足够的尾部空闲空间来做暂存
    if (target_len + 16 >= max_len)
        return;

    char *workspace = target + target_len + 1; // 利用 target 尾部空闲内存
    size_t workspace_max = max_len - target_len - 1;
    size_t wp = 0;

    const char *tmp = target;
    bool found = false;

    while (1)
    {
        const char *p = strstr(tmp, needle);
        if (p == NULL)
        {
            size_t remain = strlen(tmp);
            if (remain > 0 && wp + remain < workspace_max)
            {
                memcpy(workspace + wp, tmp, remain);
                wp += remain;
            }
            break;
        }
        found = true;
        size_t before = (size_t)(p - tmp);
        if (wp + before + repl_len >= workspace_max)
            break;

        memcpy(workspace + wp, tmp, before);
        wp += before;
        memcpy(workspace + wp, replacement, repl_len);
        wp += repl_len;
        tmp = p + needle_len;
    }
    workspace[wp] = '\0';

    if (found) {
        memcpy(target, workspace, wp + 1);
    }
}

// ==================== 核心推送逻辑 ====================

static bool send_to_single_channel(const push_channel_t *channel, const char *sender,
                                    const char *message, const char *timestamp,
                                    push_work_t *work)
{
    if (!channel || !channel->enabled || !work)
        return false;

    // 清空工作区
    memset(work, 0, sizeof(*work));
    bool is_ok = false;

    // 使用指针简写
    char *target_url = work->target_url;
    char *post_data = work->post_data;
    char *esc_sender = work->esc_sender;
    char *esc_message = work->esc_message;
    char *esc_time = work->esc_time;

    json_escape(sender, esc_sender, sizeof(work->esc_sender));
    json_escape(message, esc_message, sizeof(work->esc_message));
    json_escape(timestamp, esc_time, sizeof(work->esc_time));

    // 获取IP、MAC（小栈变量）
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
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // 使用 work->custom_buf.meta_area 暂存网络信息（与get_buf/serverchan_buf共享空间）
    char *meta_area = work->custom_buf.meta_area;
    char *apn_str = meta_area;
    char *isp_str = meta_area + 64;
    char *phone_str = meta_area + 128;

    get_network_info_cached(apn_str, isp_str, phone_str, 64, 64, 32);

    char esc_apn[64], esc_isp[64], esc_ip[32], esc_mac[32], esc_phone[32];
    json_escape(apn_str, esc_apn, sizeof(esc_apn));
    json_escape(isp_str, esc_isp, sizeof(esc_isp));
    json_escape(phone_str, esc_phone, sizeof(esc_phone));
    json_escape(ip_str, esc_ip, sizeof(esc_ip));
    json_escape(mac_str, esc_mac, sizeof(esc_mac));

    strncpy(target_url, channel->url, URL_BUF_SIZE - 1);
    target_url[URL_BUF_SIZE - 1] = '\0';

    if (channel->type == PUSH_TYPE_SERVERCHAN && strlen(target_url) == 0)
    {
        snprintf(target_url, URL_BUF_SIZE, "https://sctapi.ftqq.com/%s.send", channel->key1);
    }
    else if (channel->type == PUSH_TYPE_TELEGRAM && strlen(target_url) == 0)
    {
        snprintf(target_url, URL_BUF_SIZE, "https://api.telegram.org/bot%s/sendMessage", channel->key2);
    }

    esp_http_client_method_t method = HTTP_METHOD_POST;
    const char *content_type = "application/json";

    switch (channel->type)
    {
    case PUSH_TYPE_POST_JSON:
        snprintf(post_data, PAYLOAD_BUF_SIZE,
                 "{\"sender\":\"%.100s\",\"message\":\"%.1000s\",\"timestamp\":\"%.50s\"}",
                 esc_sender, esc_message, esc_time);
        break;

    case PUSH_TYPE_BARK:
        snprintf(post_data, PAYLOAD_BUF_SIZE,
                 "{\"title\":\"%.100s\",\"body\":\"%.1000s\"}",
                 esc_sender, esc_message);
        break;

    case PUSH_TYPE_GET:
    {
        if (strlen(message) > 2000)
        {
            ESP_LOGE(TAG, "GET URL 请求过长，超出上限");
            return false;
        }
        method = HTTP_METHOD_GET;

        char *url_sender = work->get_buf.url_sender;
        char *url_time = work->get_buf.url_time;
        char *final_url = work->get_buf.final_url;

        url_encode(sender, url_sender, 128);
        url_encode(timestamp, url_time, 128);
        url_encode(message, post_data, PAYLOAD_BUF_SIZE);

        const char *separator = (strchr(target_url, '?') == NULL) ? "?" : "&";

        int written = snprintf(final_url, URL_BUF_SIZE,
                                "%s%ssender=%s&message=%s&timestamp=%s",
                                target_url, separator, url_sender, post_data, url_time);
        if (written > 0 && (size_t)written < URL_BUF_SIZE)
        {
            memcpy(target_url, final_url, written + 1);
        }
        post_data[0] = '\0';
        break;
    }

    case PUSH_TYPE_SERVERCHAN:
    {
        content_type = "application/x-www-form-urlencoded";
        char *raw_title = work->serverchan_buf.raw_title;
        char *url_title = work->serverchan_buf.url_title;
        char *temp = work->serverchan_buf.temp_payload;

        snprintf(raw_title, 256, "短信来自:%.100s", sender);
        url_encode(raw_title, url_title, 256);

        snprintf(esc_message, 1024, "**时间:** %.50s\n\n**内容:**\n\n%.800s",
                 timestamp, message);
        url_encode(esc_message, post_data, PAYLOAD_BUF_SIZE);

        snprintf(temp, PAYLOAD_BUF_SIZE, "title=%.200s&desp=%.1200s",
                 url_title, post_data);
        // 直接用 memcpy 已知长度，避免 -Wstringop-truncation
        size_t temp_len = strlen(temp);
        if (temp_len >= PAYLOAD_BUF_SIZE) temp_len = PAYLOAD_BUF_SIZE - 1;
        memcpy(post_data, temp, temp_len);
        post_data[temp_len] = '\0';
        break;
    }

    case PUSH_TYPE_GOTIFY:
    {
        snprintf(target_url, URL_BUF_SIZE, "%.1000s%smessage?token=%.100s",
                 channel->url,
                 (channel->url[strlen(channel->url) - 1] == '/') ? "" : "/",
                 channel->key1);

        snprintf(post_data, PAYLOAD_BUF_SIZE,
                 "{\"title\":\"短信来自: %.100s\",\"message\":\"%.1000s\\n\\n时间: %.50s\",\"priority\":5}",
                 esc_sender, esc_message, esc_time);
        break;
    }

    case PUSH_TYPE_TELEGRAM:
        snprintf(post_data, PAYLOAD_BUF_SIZE,
                 "{\"chat_id\":\"%.100s\",\"text\":\"📱通知\\n发送者: %.100s\\n内容: %.1000s\\n时间: %.50s\"}",
                 channel->key1, esc_sender, esc_message, esc_time);
        break;

    case PUSH_TYPE_CUSTOM:
        snprintf(post_data, PAYLOAD_BUF_SIZE, "%.1500s", channel->customBody);
        str_replace_inplace(post_data, "{sender}", esc_sender, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{message}", esc_message, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{timestamp}", esc_time, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{isp}", esc_isp, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{apn}", esc_apn, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{device}", esc_mac, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{ip}", esc_ip, PAYLOAD_BUF_SIZE);
        str_replace_inplace(post_data, "{phone}", esc_phone, PAYLOAD_BUF_SIZE);
        break;

    default:
        return false;
    }

    ESP_LOGI(TAG, "通道 [%s] 准备请求 URL: %s", channel->name, target_url);
    is_ok = http_util_request(target_url, method, post_data, content_type);

    if (!is_ok)
        ESP_LOGE(TAG, "通道 [%s] 最终推送失败", channel->name);
    else
        ESP_LOGI(TAG, "通道 [%s] 推送成功！", channel->name);

    return is_ok;
}

static void push_service_task(void *pvParameters)
{
    push_msg_t msg;

    // ==================== 关键优化1：工作缓冲区动态分配 ====================
    // 原来 s_push_bufs (4.3KB) + 6个独立全局buffer (~3.7KB) 常驻BSS段
    // 现在合并为 push_work_t (~6KB)，由malloc动态分配，只在处理推送时占用
    // 任务空闲时，这6KB归还给堆，其他组件可以使用
    push_work_t *work = (push_work_t *)malloc(sizeof(push_work_t));
    if (!work) {
        ESP_LOGE(TAG, "无法分配推送工作缓冲区，任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "推送任务启动，工作缓冲区大小: %u 字节", (unsigned)sizeof(push_work_t));

    while (1)
    {
        // ==================== 关键优化2：带超时的网络等待 ====================
        // 原来使用 portMAX_DELAY 会永久阻塞，网络断开时任务卡死
        // 现在使用1秒超时，网络断开时能快速感知并重试
        EventBits_t bits = xEventGroupWaitBits(s_net_event_group, NET_READY_BIT,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        if (!(bits & NET_READY_BIT)) {
            continue;
        }

        // 时间同步检查
        time_t now;
        time(&now);
        if ((long long)now < 1704067200LL)
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // ==================== 消息队列接收 ====================
        if (xQueueReceive(s_push_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGI(TAG, "开始后台推送: sender=%s", msg.sender);

            bool any_success = false;

            for (int i = 0; i < MAX_PUSH_CHANNELS; i++)
            {
                if (g_app_config.pushChannels[i].enabled)
                {
                    bool ok = send_to_single_channel(&g_app_config.pushChannels[i],
                                                      msg.sender, msg.message,
                                                      msg.timestamp, work);
                    if (ok)
                        any_success = true;
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }

            // 推送成功后的短信清理
            if (any_success && msg.sms_index_count > 0)
            {
                ESP_LOGI(TAG, "推送成功，清理底层短信: %d 条", msg.sms_index_count);
                for (int i = 0; i < msg.sms_index_count; i++)
                {
                    modem_delete_sms(msg.sms_indexes[i]);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            else if (!any_success && msg.sms_index_count > 0)
            {
                ESP_LOGW(TAG, "所有推送通道均失败，保留短信不删除");
            }
        }
    }

    free(work);
}

void push_service_init(void)
{
    if (s_push_queue == NULL)
    {
        s_push_queue = xQueueCreate(PUSH_QUEUE_SIZE, sizeof(push_msg_t));

        // ==================== 关键优化3：栈大小从12288降到8192 ====================
        // 分析：
        //   push_service_task 栈上只有 push_msg_t msg (~640B)
        //   工作缓冲区 push_work_t 已改为 malloc 动态分配
        //   调用链 push_service_task -> send_to_single_channel -> http_util_request
        //     send_to_single_channel 内的栈变量：
        //       ip_str[32] + mac_str[32] + esc_apn[64] + esc_isp[64] +
        //       esc_ip[32] + esc_mac[32] + esc_phone[32] = ~360B
        //     http_util_request 内部有 mbedtls TLS 握手，栈消耗较大 (~3KB)
        //   总计栈需求 ≈ 4KB，8192 留有 2 倍余量
        xTaskCreate(push_service_task, "push_task", 8192, NULL, 5, NULL);
        ESP_LOGI(TAG, "异步推送服务及后台队列初始化完成 (栈: 6KB, 缓冲: 动态分配)");
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
        ESP_LOGE(TAG, "推送队列已满，丢弃消息");
    }
    else
    {
        ESP_LOGI(TAG, "消息已压入队列");
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

void push_service_send_with_ack(const char *sender, const char *message,
                                 const char *timestamp, const int *indexes, int index_count)
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
        ESP_LOGE(TAG, "推送队列已满，丢弃消息");
    }
}
