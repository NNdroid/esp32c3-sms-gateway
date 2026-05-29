#include "sms_processor.h"
#include "config_manager.h"
#include "pdu_decoder.h"
#include "modem_driver.h"
#include "push_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "log_service.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "SMS_PROC";

// ================= Core definitions =================
#define PDU_QUEUE_SIZE 10
#define MAX_PDU_HEX_LEN 512

#define MAX_CONCAT_PARTS 10
#define MAX_CONCAT_MESSAGES 5
#define CONCAT_TIMEOUT_MS 30000

// Queue handle
static QueueHandle_t pdu_queue;

// Single SMS part structure
typedef struct {
    bool valid;
    char text[160 * 3]; // Maximum possible UTF-8 text length
} sms_part_t;

// 长短信拼接槽位结构体
typedef struct {
    bool inUse;
    int refNumber;
    char sender[20];
    char timestamp[32];
    int totalParts;
    int receivedParts;
    TickType_t firstPartTime;
    sms_part_t parts[MAX_CONCAT_PARTS];
} concat_sms_t;

static concat_sms_t concat_buffer[MAX_CONCAT_MESSAGES];

// ================= Helper functions =================

static void normalize_phone(char *out, const char *in, size_t out_len) {
    size_t pos = 0;
    for (size_t i = 0; in[i] != '\0' && pos + 1 < out_len; i++) {
        if (isdigit((unsigned char)in[i])) {
            out[pos++] = in[i];
        } else if (in[i] == '+' && pos == 0) {
            out[pos++] = '+';
        }
    }
    out[pos] = '\0';
}

static bool phone_equals(const char *a, const char *b) {
    if (!a || !b) return false;
    char na[32] = {0};
    char nb[32] = {0};
    normalize_phone(na, a, sizeof(na));
    normalize_phone(nb, b, sizeof(nb));
    if (na[0] == '\0' || nb[0] == '\0') return false;
    if (strcmp(na, nb) == 0) return true;
    if (na[0] == '+' && strcmp(na + 1, nb) == 0) return true;
    if (nb[0] == '+' && strcmp(nb + 1, na) == 0) return true;
    return false;
}

bool sms_processor_is_admin_phone(const char* sender) {
    return sender && g_app_config.adminPhone[0] && phone_equals(sender, g_app_config.adminPhone);
}

bool sms_processor_is_blacklisted(const char* sender) {
    if (sender == NULL || sender[0] == '\0') return false;
    if (sms_processor_is_admin_phone(sender)) return false;

    if (g_app_config.numberBlackList[0] == '\0') return false;

    char list_copy[256];
    strncpy(list_copy, g_app_config.numberBlackList, sizeof(list_copy) - 1);
    list_copy[sizeof(list_copy) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(list_copy, ",; \t\r\n", &saveptr);
    while (token) {
        if (phone_equals(sender, token)) {
            return true;
        }
        token = strtok_r(NULL, ",; \t\r\n", &saveptr);
    }
    return false;
}

static void sms_processor_push(const char* sender, const char* text, const char* timestamp) {
    ESP_LOGI(TAG, "🚀 准备分发完整短信...");
    ESP_LOGI(TAG, "发件人: %s", sender);
    
    if (sms_processor_is_blacklisted(sender)) {
        ESP_LOGW(TAG, "发件人 %s 命中黑名单，丢弃该短信推送", sender);
        return;
    }

    if (sms_processor_is_admin_phone(sender)) {
        ESP_LOGI(TAG, "发件人 %s 为管理员手机号，将保留推送处理", sender);
    }

    // 触发 HTTP Webhook 推送
    push_service_send(sender, text, timestamp);
}

// ================= 长短信拼接逻辑 =================

static void init_concat_buffer(void) {
    memset(concat_buffer, 0, sizeof(concat_buffer));
}

static int find_or_create_concat_slot(int ref, const char* sdr, int tot) {
    for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
        if (concat_buffer[i].inUse && concat_buffer[i].refNumber == ref && strcmp(concat_buffer[i].sender, sdr) == 0) {
            return i;
        }
    }
    
    int oldest_slot = 0;
    TickType_t oldest_time = portMAX_DELAY;
    
    for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
        if (!concat_buffer[i].inUse) {
            concat_buffer[i].inUse = true;
            concat_buffer[i].refNumber = ref;
            strlcpy(concat_buffer[i].sender, sdr, sizeof(concat_buffer[i].sender));
            concat_buffer[i].totalParts = tot;
            concat_buffer[i].receivedParts = 0;
            concat_buffer[i].firstPartTime = xTaskGetTickCount();
            for(int j=0; j<MAX_CONCAT_PARTS; j++) concat_buffer[i].parts[j].valid = false;
            return i;
        }
        // 记录最老的槽位，以便在缓冲满时淘汰
        if (concat_buffer[i].firstPartTime < oldest_time) {
            oldest_time = concat_buffer[i].firstPartTime;
            oldest_slot = i;
        }
    }
    
    // 强制淘汰最老的槽位 (极少发生)
    ESP_LOGW(TAG, "拼接缓冲已满，淘汰旧数据 (Ref: %d)", concat_buffer[oldest_slot].refNumber);
    concat_buffer[oldest_slot].inUse = true;
    concat_buffer[oldest_slot].refNumber = ref;
    strlcpy(concat_buffer[oldest_slot].sender, sdr, sizeof(concat_buffer[oldest_slot].sender));
    concat_buffer[oldest_slot].totalParts = tot;
    concat_buffer[oldest_slot].receivedParts = 0;
    concat_buffer[oldest_slot].firstPartTime = xTaskGetTickCount();
    for(int j=0; j<MAX_CONCAT_PARTS; j++) concat_buffer[oldest_slot].parts[j].valid = false;
    
    return oldest_slot; 
}

// ================= 后台处理任务 =================

static void sms_processor_task(void *pvParameters) {
    char pdu_hex[MAX_PDU_HEX_LEN];
    decoded_sms_t decoded_sms;

    init_concat_buffer();
    ESP_LOGI(TAG, "短信处理与分发引擎已启动");

    for(;;) {
        // Wait for PDU queue data (timeout 2 seconds)
        if (xQueueReceive(pdu_queue, pdu_hex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            
            // 1. 解码 PDU
            if (pdu_decode(pdu_hex, &decoded_sms) == ESP_OK) {
                
                // 2. 判断是否是长短信分片
                if (decoded_sms.is_concat && decoded_sms.total_parts > 1 && decoded_sms.part_number > 0) {
                    ESP_LOGI(TAG, "收到长短信分片: 序号 %d/%d, 标识 %d", 
                             decoded_sms.part_number, decoded_sms.total_parts, decoded_sms.ref_number);
                             
                    int slot = find_or_create_concat_slot(decoded_sms.ref_number, decoded_sms.sender, decoded_sms.total_parts);
                    int pIdx = decoded_sms.part_number - 1;
                    
                    if (pIdx >= 0 && pIdx < MAX_CONCAT_PARTS && !concat_buffer[slot].parts[pIdx].valid) {
                        concat_buffer[slot].parts[pIdx].valid = true;
                        strlcpy(concat_buffer[slot].parts[pIdx].text, decoded_sms.text, sizeof(concat_buffer[slot].parts[pIdx].text));
                        concat_buffer[slot].receivedParts++;
                        
                        if (concat_buffer[slot].receivedParts == 1) {
                            strlcpy(concat_buffer[slot].timestamp, decoded_sms.timestamp, sizeof(concat_buffer[slot].timestamp));
                        }
                    }
                    
                    // 3. 检查是否拼接完成
                    if (concat_buffer[slot].receivedParts >= decoded_sms.total_parts) {
                        ESP_LOGI(TAG, "✅ 长短信拼接完成");
                        
                        // 🚀 修复点 1：使用堆内存代替栈内存
                        char *full_text = calloc(1, 160 * 3 * MAX_CONCAT_PARTS);
                        if (full_text) {
                            for (int i = 0; i < decoded_sms.total_parts; i++) {
                                if (concat_buffer[slot].parts[i].valid) {
                                    strcat(full_text, concat_buffer[slot].parts[i].text);
                                } else {
                                    strcat(full_text, "[缺分段]");
                                }
                            }
                            sms_processor_push(concat_buffer[slot].sender, full_text, concat_buffer[slot].timestamp);
                            free(full_text); // 🚀 用完立刻释放
                        } else {
                            ESP_LOGE(TAG, "内存不足，无法拼接长短信");
                        }
                        concat_buffer[slot].inUse = false; // 清空槽位
                    }
                } else {
                    // Regular single SMS
                    ESP_LOGI(TAG, "收到普通短信");
                    sms_processor_push(decoded_sms.sender, decoded_sms.text, decoded_sms.timestamp);
                }
            } else {
                ESP_LOGE(TAG, "PDU 字符串解码失败");
            }
        }
        
        // 4. 检查超时未完成的长短信槽位
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
            if (concat_buffer[i].inUse) {
                uint32_t elapsed_ms = (now - concat_buffer[i].firstPartTime) * portTICK_PERIOD_MS;
                if (elapsed_ms >= CONCAT_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "⚠️ 长短信等待分片超时 (已过 %d 毫秒)，强制转发已收到的部分", CONCAT_TIMEOUT_MS);
                    
                    // 🚀 修复点 2：使用堆内存代替栈内存
                    char *full_text = calloc(1, 160 * 3 * MAX_CONCAT_PARTS);
                    if (full_text) {
                        for (int j = 0; j < concat_buffer[i].totalParts; j++) {
                            if (concat_buffer[i].parts[j].valid) {
                                strcat(full_text, concat_buffer[i].parts[j].text);
                            } else {
                                strcat(full_text, "\n[缺失的短信分段]\n");
                            }
                        }
                        sms_processor_push(concat_buffer[i].sender, full_text, concat_buffer[i].timestamp);
                        free(full_text); // 🚀 用完立刻释放
                    }
                    concat_buffer[i].inUse = false; // 清空槽位
                }
            }
        }
    }
}

// ================= API 暴露 =================

esp_err_t sms_processor_enqueue_pdu(const char* pdu_hex_str) {
    if (strlen(pdu_hex_str) >= MAX_PDU_HEX_LEN) return ESP_ERR_INVALID_SIZE;
    if (xQueueSend(pdu_queue, pdu_hex_str, 0) == pdTRUE) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "PDU 队列已满，丢弃数据！");
    return ESP_FAIL;
}

static void modem_sms_urc_handler(void* handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base != MODEM_EVENT || event_id != MODEM_EVENT_SMS_RECEIVED) {
        return;
    }
    if (!event_data) {
        return;
    }

    // 从 event_data 中提取 PDU（前提是你的 modem_driver 确实将 +CMT: 行和 PDU 行拼在了一起发过来）
    const char* payload = (const char*) event_data;
    const char* pdu_line = strchr(payload, '\n');
    if (!pdu_line) {
        ESP_LOGW(TAG, "SMS URC 数据不完整，未找到换行符，无法提取 PDU");
        return;
    }
    pdu_line++; // 跳过换行符，指向 PDU 开头

    size_t len = strlen(pdu_line);
    // 安全剔除末尾可能存在的 \r 或 \n
    while (len > 0 && (pdu_line[len - 1] == '\r' || pdu_line[len - 1] == '\n')) {
        len--; 
    }

    if (len == 0) {
        ESP_LOGW(TAG, "SMS URC 中未包含 PDU 内容");
        return;
    }

    char pdu[MAX_PDU_HEX_LEN] = {0};
    if (len >= sizeof(pdu)) {
        len = sizeof(pdu) - 1;
    }
    memcpy(pdu, pdu_line, len);
    pdu[len] = '\0';

    ESP_LOGI(TAG, "成功提取 PDU，长度: %d", (int)len);
    sms_processor_enqueue_pdu(pdu);
}

void sms_processor_init(void) {
    // 创建队列
    pdu_queue = xQueueCreate(PDU_QUEUE_SIZE, MAX_PDU_HEX_LEN);
    if (!pdu_queue) {
        ESP_LOGE(TAG, "创建 PDU 队列失败");
        return;
    }

    // 注册 SMS URC 事件监听
    esp_event_handler_instance_register(MODEM_EVENT, MODEM_EVENT_SMS_RECEIVED, &modem_sms_urc_handler, NULL, NULL);
    
    // 启动后台任务，分配 8KB 堆栈
    xTaskCreate(sms_processor_task, "sms_proc_task", 10240, NULL, 4, NULL);
}