#include "cron_tasker.h"
#include "config_manager.h"
#include "modem_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "log_service.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "CRON_MGR";

// 外部引用，用于将定时任务的结果推送到你的 Webhook
extern void push_service_send(const char* sender, const char* message, const char* timestamp);

/**
 * @brief 计算下一次执行的绝对 Unix 时间戳 (秒)
 */
static unsigned long calculate_next_run_epoch(int hour, int minute, int days_offset) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = 0;
    timeinfo.tm_mday += days_offset;

    time_t target_epoch = mktime(&timeinfo);

    if (target_epoch <= now && days_offset == 0) {
        timeinfo.tm_mday += 1;
        target_epoch = mktime(&timeinfo);
    }

    return (unsigned long)target_epoch;
}

/**
 * @brief 定时任务后台守护线程
 */
static void cron_worker_task(void *pvParameters) {
    ESP_LOGI(TAG, "⏰ 定时任务守护线程已启动...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60*1000));

        time_t now;
        time(&now);

        if (now < 1780016300) {
            ESP_LOGW(TAG, "NTP 尚未同步，定时任务暂挂...");
            continue;
        }

        bool config_updated = false;

        for (int i = 0; i < MAX_CRON_TASKS; i++) {
            if (!g_app_config.cronTasks[i].enabled) continue;
            //ESP_LOGI(TAG, "🚀 [精准触发] 定时任务 %d 开始执行", i + 1);

            if (g_app_config.cronTasks[i].nextRunEpoch == 0) {
                g_app_config.cronTasks[i].nextRunEpoch = calculate_next_run_epoch(
                    g_app_config.cronTasks[i].hour, 
                    g_app_config.cronTasks[i].minute, 
                    0
                );
                config_updated = true;
                ESP_LOGI(TAG, "任务 %d 初始化，预定于 %lu 执行", i + 1, g_app_config.cronTasks[i].nextRunEpoch);
                continue;
            }

            if ((unsigned long)now >= g_app_config.cronTasks[i].nextRunEpoch) {
                // 🛡️ 保护机制：检查上次执行时间，防止一分钟内重复触发
                // (如果你的任务不需要这么频繁，这个保护是必须的)
                if (now - g_app_config.cronTasks[i].nextRunEpoch < 60) {
                     // 任务刚刚执行过，虽然时间到了，但还没到下一个周期，跳过
                     continue; 
                }

                ESP_LOGI(TAG, "🚀 [精准触发] 定时任务 %d 开始执行", i + 1);

                // 🚀 修复点 1：将大块内存转移到堆 (Heap) 上
                char *report_msg = calloc(1, 1024);
                if (!report_msg) {
                    ESP_LOGE(TAG, "内存不足，放弃本次定时任务");
                    continue;
                }

                // 生成当前时间戳字符串，供推送服务使用
                char time_str[32] = {0};
                struct tm timeinfo;
                localtime_r(&now, &timeinfo);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

                if (g_app_config.cronTasks[i].type == 0) {
                    ESP_LOGI(TAG, "🚀 定时任务 %d：开始执行 Ping 测试", i + 1);
                    // ==============================
                    // 任务类型 0：网络 Ping 测试
                    // ==============================
                    const char *target = g_app_config.cronTasks[i].pingTarget;
                    if (strlen(target) == 0) target = "8.8.8.8";

                    int successCount = 0;
                    int avgRtt = 0;
                    char details[512] = {0};

                    ESP_LOGI(TAG, "任务 %d：准备调用 modem_ping...", i + 1);
                    esp_err_t err = modem_ping(target, 4, 30, &successCount, &avgRtt, details, sizeof(details));
                    ESP_LOGI(TAG, "任务 %d：modem_ping 返回，结果: 0x%X", i + 1, err);
                    if (err == ESP_OK) {
                        snprintf(report_msg, 1024, "【定时任务 %d：Ping 测试】\n目标：%s\n结果：%s\n成功：%d/4\n延时：%d ms\n%s", 
                                 i + 1, target, (successCount > 0) ? "✅ 成功" : "❌ 失败", successCount, avgRtt, details);
                    } else {
                        snprintf(report_msg, 1024, "【定时任务 %d：Ping 测试】\n目标：%s\n结果：❌ 失败\n错误码：0x%X", i + 1, target, err);
                    }
                    push_service_send("SYSTEM", report_msg, time_str);
                    ESP_LOGI(TAG, "%s", report_msg);

                } else if (g_app_config.cronTasks[i].type == 1) {
                    ESP_LOGI(TAG, "🚀 定时任务 %d：开始执行发送短信", i + 1);
                    // ==============================
                    // 任务类型 1：发送定时短信
                    // ==============================
                    const char *phone = g_app_config.cronTasks[i].phone;
                    const char *content = g_app_config.cronTasks[i].content;
                    if (strlen(content) == 0) content = "设备定时报告：运作正常";

                    esp_err_t sms_err = modem_send_sms_text(phone, content, 10000);
                    
                    snprintf(report_msg, 1024, "【定时任务 %d：发送短信】\n接收号码：%s\n执行结果：%s", 
                             i + 1, phone, (sms_err == ESP_OK) ? "✅ 成功" : "❌ 失败");
                    
                    push_service_send("SYSTEM", report_msg, time_str);
                    ESP_LOGI(TAG, "%s", report_msg);
                } else {
                    ESP_LOGW(TAG, "未知的定时任务类型: %d", g_app_config.cronTasks[i].type);
                }

                free(report_msg); // 🚀 推送完成后释放 report_msg

                if (g_app_config.cronTasks[i].daysInterval > 0) {
                    g_app_config.cronTasks[i].nextRunEpoch = calculate_next_run_epoch(
                        g_app_config.cronTasks[i].hour, 
                        g_app_config.cronTasks[i].minute, 
                        g_app_config.cronTasks[i].daysInterval
                    );
                    ESP_LOGI(TAG, "任务 %d 下次执行时间: %lu", i + 1, g_app_config.cronTasks[i].nextRunEpoch);
                } else {
                    g_app_config.cronTasks[i].enabled = false;
                    g_app_config.cronTasks[i].nextRunEpoch = 0;
                    ESP_LOGI(TAG, "任务 %d 已完成。", i + 1);
                }
                
                config_updated = true;
            }
        }

        if (config_updated) {
            config_save_all();
        }
    }
}

esp_err_t cron_tasker_init(void) {
    BaseType_t ret = xTaskCreate(cron_worker_task, "cron_worker", 8192, NULL, 2, NULL);
    if (ret == pdPASS) {
        return ESP_OK;
    }
    return ESP_FAIL;
}