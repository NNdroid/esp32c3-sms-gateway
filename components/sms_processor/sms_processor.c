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
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "SMS_PROC";

// ================= Core definitions =================
#define EVENT_QUEUE_SIZE 20
#define MAX_PDU_HEX_LEN 512

#define MAX_CONCAT_PARTS 10
#define MAX_CONCAT_MESSAGES 5
#define CONCAT_TIMEOUT_MS 30000

// 双模兼容事件结构体
typedef struct {
    bool is_stored;                // true: 存储模式 (只有 index), false: 直通模式 (包含完整 PDU)
    int sms_index;                 // SIM卡/模块槽位索引
    char pdu_hex[MAX_PDU_HEX_LEN]; // 完整的 PDU 字符串
} sms_event_t;

// Queue handle
static QueueHandle_t sms_event_queue;

// Single SMS part structure
typedef struct {
    bool valid;
    int sms_index;
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

// concat_buffer 改为指针，由 malloc 动态分配，不常驻BSS段
// 长短信拼接在短信推送中是小概率事件，没必要常驻 ~24KB
static concat_sms_t *concat_buffer = NULL;

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

// ================= 长短信拼接逻辑 =================

static void init_concat_buffer(void) {
    if (!concat_buffer) {
        concat_buffer = (concat_sms_t *)calloc(MAX_CONCAT_MESSAGES, sizeof(concat_sms_t));
        if (!concat_buffer) {
            ESP_LOGE(TAG, "无法分配长短信拼接缓冲区！长短信功能将不可用");
        }
    } else {
        memset(concat_buffer, 0, MAX_CONCAT_MESSAGES * sizeof(concat_sms_t));
    }
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
        if (concat_buffer[i].firstPartTime < oldest_time) {
            oldest_time = concat_buffer[i].firstPartTime;
            oldest_slot = i;
        }
    }
    
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
    sms_event_t evt;
    char pdu_hex_buf[MAX_PDU_HEX_LEN]; 
    decoded_sms_t decoded_sms;

    init_concat_buffer();
    ESP_LOGI(TAG, "✅ 短信处理与分发引擎已启动 (双模兼容: 存储拉取 + 直通)");

    for(;;) {
        // 从队列获取事件 (超时 2 秒以便处理长短信合并超时)
        if (xQueueReceive(sms_event_queue, &evt, pdMS_TO_TICKS(2000)) == pdTRUE) {
            
            bool pdu_ready = false;

            // ---------------- 核心兼容逻辑 ----------------
            if (evt.is_stored) {
                ESP_LOGI(TAG, "🔍 正在向模块读取槽位 %d 的短信内容...", evt.sms_index);
                
                if (modem_read_sms_pdu(evt.sms_index, pdu_hex_buf, sizeof(pdu_hex_buf)) == ESP_OK) {
                    pdu_ready = true;
                } else {
                    ESP_LOGE(TAG, "❌ 读取槽位 %d 失败，放弃该短信", evt.sms_index);
                }
            } else {
                strlcpy(pdu_hex_buf, evt.pdu_hex, sizeof(pdu_hex_buf));
                pdu_ready = true;
            }
            // ----------------------------------------------

            // 统一进入解码与拼接流程
            if (pdu_ready) {
                if (pdu_decode(pdu_hex_buf, &decoded_sms) == ESP_OK) {
                    
                    // 黑名单过滤提前拦截
                    if (sms_processor_is_blacklisted(decoded_sms.sender)) {
                        ESP_LOGW(TAG, "拦截: 发件人 %s 命中黑名单，丢弃并自动清理该短信", decoded_sms.sender);
                        if (evt.is_stored) {
                            modem_delete_sms(evt.sms_index); // 黑名单垃圾短信直接底层清理
                        }
                        continue; 
                    }

                    if (decoded_sms.is_concat && decoded_sms.total_parts > 1 && decoded_sms.part_number > 0) {
                        ESP_LOGI(TAG, "🧩 收到长短信分片: 序号 %d/%d, 标识 %d", 
                                 decoded_sms.part_number, decoded_sms.total_parts, decoded_sms.ref_number);
                                 
                        int slot = find_or_create_concat_slot(decoded_sms.ref_number, decoded_sms.sender, decoded_sms.total_parts);
                        int pIdx = decoded_sms.part_number - 1;
                        
                        if (pIdx >= 0 && pIdx < MAX_CONCAT_PARTS && !concat_buffer[slot].parts[pIdx].valid) {
                            concat_buffer[slot].parts[pIdx].valid = true;
                            strlcpy(concat_buffer[slot].parts[pIdx].text, decoded_sms.text, sizeof(concat_buffer[slot].parts[pIdx].text));
                            concat_buffer[slot].parts[pIdx].sms_index = evt.sms_index;
                            concat_buffer[slot].receivedParts++;
                            
                            if (concat_buffer[slot].receivedParts == 1) {
                                strlcpy(concat_buffer[slot].timestamp, decoded_sms.timestamp, sizeof(concat_buffer[slot].timestamp));
                            }
                        }
                        
                                                if (concat_buffer[slot].receivedParts >= decoded_sms.total_parts) {
                                                    ESP_LOGI(TAG, "长短信拼接完成，准备推送");
                                                    // static 局部变量，随 task 常驻，避免栈溢出
                                                    // full_text[4800] + concat_buffer ~24KB 分别从栈和 BSS 解放
                                                    static char full_text[160 * 3 * MAX_CONCAT_PARTS];
                            full_text[0] = '\0';
                            int indexes_to_delete[MAX_CONCAT_PARTS];
                            int del_count = 0;
                            
                            for (int i = 0; i < decoded_sms.total_parts; i++) {
                                if (concat_buffer[slot].parts[i].valid) {
                                    strlcat(full_text, concat_buffer[slot].parts[i].text, sizeof(full_text));
                                    if (evt.is_stored) {
                                        indexes_to_delete[del_count++] = concat_buffer[slot].parts[i].sms_index;
                                    }
                                }
                            }
                            
                            push_service_send_with_ack(concat_buffer[slot].sender, full_text, concat_buffer[slot].timestamp, 
                                                       evt.is_stored ? indexes_to_delete : NULL, del_count);
                            concat_buffer[slot].inUse = false; 
                        }
                    } else {
                        // Regular single SMS
                        ESP_LOGI(TAG, "💬 收到普通完整短信，发件人: %s，开始推送", decoded_sms.sender);
                        int single_index[1] = { evt.sms_index };
                        push_service_send_with_ack(decoded_sms.sender, decoded_sms.text, decoded_sms.timestamp, 
                                                   evt.is_stored ? single_index : NULL, evt.is_stored ? 1 : 0);
                    }
                } else {
                    ESP_LOGE(TAG, "❌ PDU 字符串解码失败:\n%s", pdu_hex_buf);
                    // 如果乱码或者无法解码，可以直接删掉防止堵塞内存
                    if (evt.is_stored) { modem_delete_sms(evt.sms_index); }
                }
            }
        }
        
        // 超时未完成的长短信检查
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
            if (concat_buffer[i].inUse) {
                uint32_t elapsed_ms = (now - concat_buffer[i].firstPartTime) * portTICK_PERIOD_MS;
                if (elapsed_ms >= CONCAT_TIMEOUT_MS) {
                                                                                ESP_LOGW(TAG, "长短信等待分片超时，强制转发已收到的部分");
                    
                    static char full_text[160 * 3 * MAX_CONCAT_PARTS];
                    full_text[0] = '\0';
                    int indexes_to_delete[MAX_CONCAT_PARTS];
                    int del_count = 0;

                    for (int j = 0; j < concat_buffer[i].totalParts; j++) {
                        if (concat_buffer[i].parts[j].valid) {
                            strlcat(full_text, concat_buffer[i].parts[j].text, sizeof(full_text));
                            indexes_to_delete[del_count++] = concat_buffer[i].parts[j].sms_index;
                        } else {
                            strlcat(full_text, "\n[缺失的短信分段]\n", sizeof(full_text));
                        }
                    }
                    push_service_send_with_ack(concat_buffer[i].sender, full_text, concat_buffer[i].timestamp, 
                                               (del_count > 0) ? indexes_to_delete : NULL, del_count);
                    concat_buffer[i].inUse = false;
                }
            }
        }
    }
}

// ================= API 暴露 =================

esp_err_t sms_processor_enqueue_pdu(const char* pdu_hex_str) {
    if (strlen(pdu_hex_str) >= MAX_PDU_HEX_LEN) return ESP_ERR_INVALID_SIZE;
    
    sms_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.is_stored = false;
    strlcpy(evt.pdu_hex, pdu_hex_str, sizeof(evt.pdu_hex));

    if (xQueueSend(sms_event_queue, &evt, 0) == pdTRUE) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "事件队列已满，丢弃数据！");
    return ESP_FAIL;
}

// ================= URC 回调：智能双模识别 =================
static void modem_sms_urc_handler(void* handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base != MODEM_EVENT || event_id != MODEM_EVENT_SMS_RECEIVED || !event_data) {
        return;
    }

    const char* payload = (const char*) event_data;
    sms_event_t evt;
    memset(&evt, 0, sizeof(evt));

    ESP_LOGI(TAG, "开始解析 URC...");
    if (strstr(payload, "+CMTI:") != NULL) {
        const char *comma = strchr(payload, ',');
        if (comma != NULL) {
            evt.sms_index = atoi(comma + 1); // 提取逗号后面的数字
            evt.is_stored = true;
            
            ESP_LOGI(TAG, "📩 [存储模式] 成功解析到新短信位于槽位: %d", evt.sms_index);
            ESP_LOGI(TAG, "解析 CMTI 完成，准备压入队列");
            if (xQueueSend(sms_event_queue, &evt, 0) != pdTRUE) {
                ESP_LOGE(TAG, "❌ 事件队列已满，丢失通知！(索引: %d)", evt.sms_index);
                return;
            }
            ESP_LOGI(TAG, "压入队列成功！");
        } else {
            ESP_LOGW(TAG, "⚠️ 收到 +CMTI 提示，但无法找到逗号解析索引: %s", payload);
        }
        return; // 处理完毕，直接退出
    }

    // 模式 B：检测是否为直通模式 (+CMT: ...)
    const char* pdu_line = strchr(payload, '\n');
    if (pdu_line && strstr(payload, "+CMT:") != NULL) {
        pdu_line++; // 跳过换行符

        size_t len = strlen(pdu_line);
        while (len > 0 && (pdu_line[len - 1] == '\r' || pdu_line[len - 1] == '\n')) {
            len--; 
        }

        if (len > 0 && len < MAX_PDU_HEX_LEN) {
            evt.is_stored = false;
            memcpy(evt.pdu_hex, pdu_line, len);
            evt.pdu_hex[len] = '\0';
            
            ESP_LOGI(TAG, "📩 [直通模式] 提取 PDU，长度: %d", (int)len);
            
            if (xQueueSend(sms_event_queue, &evt, 0) != pdTRUE) {
                ESP_LOGE(TAG, "❌ 事件队列已满，丢失直通 PDU！");
            }
        } else {
            ESP_LOGW(TAG, "直通 PDU 提取失败：长度异常 (%d)", (int)len);
        }
    }
}

void sms_processor_retry_failed_pushes(void) {
    ESP_LOGI(TAG, "🧹 开始扫描并尝试重推未删除的遗留短信...");
    
    int indices[50]; // 每次最多捞 50 条防止内存挤兑
    int count = 0;
    
    if (modem_get_all_sms_indices(indices, 50, &count) == ESP_OK) {
        if (count == 0) {
            ESP_LOGI(TAG, "🎉 当前无遗留短信，系统非常干净");
            return;
        }
        
        ESP_LOGI(TAG, "📦 共找到 %d 条遗留短信，准备放入队列重试...", count);
        for (int i = 0; i < count; i++) {
            sms_event_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.is_stored = true;
            evt.sms_index = indices[i];
            
            // 压入队列 (稍微给点延时防止队列被瞬间塞满)
            if (xQueueSend(sms_event_queue, &evt, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "⚠️ 事件队列已满，遗留短信(槽位 %d)将留待下次轮询", indices[i]);
            }
        }
    } else {
        ESP_LOGE(TAG, "❌ 获取遗留短信列表失败");
    }
}

void sms_processor_init(void) {
    // 创建双模事件队列
    sms_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(sms_event_t));
    if (!sms_event_queue) {
        ESP_LOGE(TAG, "创建事件队列失败");
        return;
    }

    esp_event_handler_instance_register(MODEM_EVENT, MODEM_EVENT_SMS_RECEIVED, &modem_sms_urc_handler, NULL, NULL);
    
                // 启动处理 Task
    xTaskCreate(sms_processor_task, "sms_proc_task", 5120, NULL, 4, NULL);
}