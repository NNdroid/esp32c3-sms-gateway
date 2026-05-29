#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "log_service.h"
#include <string.h>

static const char *TAG = "CONFIG_MGR";
#define NVS_NAMESPACE "sms_config"

// 实例化全局配置变量
app_config_t g_app_config;

// 辅助函数：安全读取字符串 (如果 NVS 中没有该键，则使用默认值)
static void nvs_get_string_safe(nvs_handle_t handle, const char* key, char* out_str, size_t max_len, const char* default_val) {
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len) {
        nvs_get_str(handle, key, out_str, &required_size);
    } else {
        // 如果键不存在或长度超过缓冲，写入默认值
        strncpy(out_str, default_val, max_len - 1);
        out_str[max_len - 1] = '\0';
    }
}

// 初始化 NVS 底层
void config_manager_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区异常或版本不符，执行擦除并重试
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS 初始化成功");
}

// 载入全部配置
void config_load_all(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间失败");
        return;
    }

    // 1. 读取基础字符串变量
    nvs_get_string_safe(my_handle, "adminPhone", g_app_config.adminPhone, sizeof(g_app_config.adminPhone), "");
    nvs_get_string_safe(my_handle, "webUser", g_app_config.webUser, sizeof(g_app_config.webUser), "admin");
    nvs_get_string_safe(my_handle, "webPass", g_app_config.webPass, sizeof(g_app_config.webPass), "admin123");
    nvs_get_string_safe(my_handle, "numBlkList", g_app_config.numberBlackList, sizeof(g_app_config.numberBlackList), "");
    
    nvs_get_string_safe(my_handle, "syslogSrv", g_app_config.syslogServer, sizeof(g_app_config.syslogServer), "");
    nvs_get_string_safe(my_handle, "netPlmn", g_app_config.plmn, sizeof(g_app_config.plmn), "AUTO");
    nvs_get_string_safe(my_handle, "netSmsc", g_app_config.smsc, sizeof(g_app_config.smsc), "");
    nvs_get_string_safe(my_handle, "netImei", g_app_config.imei, sizeof(g_app_config.imei), "");

    // 2. 读取整型 / 布尔型
    int8_t syslog_en = 0;
    if (nvs_get_i8(my_handle, "syslogEn", &syslog_en) == ESP_OK) {
        g_app_config.syslogEnabled = (syslog_en == 1);
    } else {
        g_app_config.syslogEnabled = false; // 默认值
    }

    int32_t syslog_port = 514;
    if (nvs_get_i32(my_handle, "syslogPort", &syslog_port) == ESP_OK) {
        g_app_config.syslogPort = syslog_port;
    } else {
        g_app_config.syslogPort = 514; // 默认值
    }

    // 3. 批量读取结构体数组 (Push Channels)
    size_t channels_size = sizeof(g_app_config.pushChannels);
    err = nvs_get_blob(my_handle, "push_channels", g_app_config.pushChannels, &channels_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "未找到推送通道配置，将使用默认空配置");
        memset(g_app_config.pushChannels, 0, sizeof(g_app_config.pushChannels));
    }

    // 4. 批量读取结构体数组 (Cron Tasks)
    size_t cron_size = sizeof(g_app_config.cronTasks);
    err = nvs_get_blob(my_handle, "cron_tasks", g_app_config.cronTasks, &cron_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "未找到定时任务配置，将使用默认空配置");
        memset(g_app_config.cronTasks, 0, sizeof(g_app_config.cronTasks));
    }

    nvs_close(my_handle);
    ESP_LOGI(TAG, "系统配置载入完毕");
}

// 保存全部配置
void config_save_all(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 以保存配置失败");
        return;
    }

    // 1. 保存基础字符串变量
    nvs_set_str(my_handle, "adminPhone", g_app_config.adminPhone);
    nvs_set_str(my_handle, "webUser", g_app_config.webUser);
    nvs_set_str(my_handle, "webPass", g_app_config.webPass);
    nvs_set_str(my_handle, "numBlkList", g_app_config.numberBlackList);
    
    nvs_set_str(my_handle, "syslogSrv", g_app_config.syslogServer);
    nvs_set_str(my_handle, "netPlmn", g_app_config.plmn);
    nvs_set_str(my_handle, "netSmsc", g_app_config.smsc);
    nvs_set_str(my_handle, "netImei", g_app_config.imei);

    // 2. 保存整型 / 布尔型
    nvs_set_i8(my_handle, "syslogEn", g_app_config.syslogEnabled ? 1 : 0);
    nvs_set_i32(my_handle, "syslogPort", g_app_config.syslogPort);

    // 3. 批量保存结构体数组 (Push Channels)
    nvs_set_blob(my_handle, "push_channels", g_app_config.pushChannels, sizeof(g_app_config.pushChannels));

    // 4. 批量保存结构体数组 (Cron Tasks)
    nvs_set_blob(my_handle, "cron_tasks", g_app_config.cronTasks, sizeof(g_app_config.cronTasks));

    // 提交更改
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置提交失败");
    } else {
        ESP_LOGI(TAG, "系统配置已成功保存至 NVS");
    }
    
    nvs_close(my_handle);
}