#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "log_service.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "driver/gpio.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

#include "config_manager.h"
#include "modem_driver.h"
#include "push_service.h"
#include "cron_tasker.h"
#include "sms_processor.h"
#include "call_processor.h"
#include "web_server.h"

static const char *TAG = "APP_MAIN";

// 定义 LED 引脚
#define LED_BUILTIN 8
#define LED_ON  0
#define LED_OFF 1

// 配网配置
#define MAX_RETRY_WIFI 15
#define PROV_PIN "20260529"

static int s_retry_num = 0;
static bool s_provisioning_started = false;

// 前置声明
static void start_ble_provisioning(void);

void time_sync_notification_cb(struct timeval *tv) {
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "⏰ 收到底层 NTP 同步完成事件: %s", strftime_buf);
}

static void ntp_time_init(void) {
    ESP_LOGI(TAG, "初始化 SNTP 服务 (异步模式)...");
    
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_servermode_dhcp(1);

    esp_sntp_setservername(0, "ntp.ntsc.ac.cn"); 
    esp_sntp_setservername(1, "time.apple.com"); 
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_init();
    
    setenv("TZ", "CST-8", 1);
    tzset();
    
    ESP_LOGI(TAG, "SNTP 初始化完成，已转入后台自动同步");
}

// 配网事件回调函数
static void wifi_prov_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "📡 蓝牙配网服务已启动，等待 App 连接...");
                break;
            case NETWORK_PROV_WIFI_CRED_RECV:
                ESP_LOGI(TAG, "📥 收到手机下发的 Wi-Fi 凭据，准备尝试连接...");
                break;
            case NETWORK_PROV_WIFI_CRED_FAIL:
                ESP_LOGE(TAG, "❌ 给定的 Wi-Fi 凭据验证失败 (密码错误或找不到SSID)！重置配网状态等待重新输入...");
                // 清除错误状态，允许用户在 App 端重新输入密码
                network_prov_mgr_reset_wifi_sm_state_on_failure();
                break;
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "✅ Wi-Fi 凭据验证成功，已连接到路由器");
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "🎉 配网流程结束，释放蓝牙底层资源");
                // 释放管理器，释放 BLE 内存
                network_prov_mgr_deinit();
                s_provisioning_started = false;
                break;
            default:
                break;
        }
    }
}

// 常规 Wi-Fi 事件回调
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 只有在非配网模式下，开机才主动发起连接。
        // 防止配网模式下拿着空的 SSID 去连路由器。
        if (!s_provisioning_started) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        log_service_set_ready(false);
        
        // 如果配网服务正在运行，由 network_prov_mgr 内部接管重连尝试，不手动调用 esp_wifi_connect
        if (s_provisioning_started) {
            return;
        }

        if (s_retry_num < MAX_RETRY_WIFI) {
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi 断开连接，正在重连... (%d/%d)", s_retry_num, MAX_RETRY_WIFI);
            esp_wifi_connect(); 
        } else {
            ESP_LOGE(TAG, "连续 %d 次连接失败，触发蓝牙配网模式！", MAX_RETRY_WIFI);
            start_ble_provisioning();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ 成功获取局域网 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; // 重置重试计数器
        log_service_set_ready(true);
        ntp_time_init();
    }
}

// (venv) PS E:\SourceTreeProjects\esp32c3-sms-gateway> python E:\SourceTreeProjects\esp32c3-sms-gateway\managed_components\espressif__network_provisioning\tool\esp_prov\esp_prov.py --transport ble --sec_ver 2 --sec2_gen_cred --sec2_username prov --sec2_pwd 20260529
// ==== Salt-verifier for security scheme 2 (SRP6a) ====
static const char sec2_salt[] = {
    0xa4, 0xc1, 0x61, 0x41, 0x5e, 0xb3, 0x9d, 0xa8, 0x57, 0xb8, 0xa2, 0x35, 0xd1, 0x5b, 0x63, 0x19
};

static const char sec2_verifier[] = {
    0x01, 0xcb, 0x18, 0xa5, 0x3d, 0x7a, 0xa0, 0xb2, 0xe0, 0x6a, 0x17, 0x69, 0x27, 0x25, 0xf7, 0xb8, 
    0xa5, 0x81, 0x10, 0x3f, 0x34, 0xc6, 0xd2, 0xba, 0x53, 0x13, 0xd6, 0x14, 0x57, 0xd6, 0x16, 0x83, 
    0xc3, 0xb0, 0xa7, 0x47, 0x49, 0x89, 0x6a, 0xb2, 0x00, 0xd2, 0x9b, 0x35, 0xa7, 0x08, 0xcb, 0xa7, 
    0xce, 0xac, 0x72, 0xa7, 0xcc, 0x8c, 0x21, 0x29, 0xd7, 0x3e, 0x1b, 0x71, 0x8d, 0x7f, 0xf3, 0x1d, 
    0xb9, 0x94, 0x15, 0x9f, 0x3b, 0x4b, 0x3e, 0x6a, 0xf9, 0x58, 0x9a, 0x1a, 0x36, 0x51, 0xc9, 0x5b, 
    0x0b, 0x7f, 0x47, 0x9d, 0x72, 0x50, 0x12, 0x42, 0x0c, 0xf4, 0x7d, 0x3c, 0x81, 0x71, 0x38, 0x5f, 
    0xba, 0x1d, 0x8b, 0x85, 0x78, 0x7c, 0x43, 0x62, 0x48, 0xd7, 0xc7, 0xba, 0x4b, 0x73, 0xea, 0x82, 
    0x42, 0x41, 0x34, 0x7a, 0x82, 0xf6, 0x41, 0x0c, 0x3e, 0x55, 0x56, 0x3a, 0x0c, 0x11, 0x7a, 0x3c, 
    0x95, 0x1f, 0xeb, 0x7b, 0x13, 0xad, 0xaf, 0x7b, 0x2a, 0xf6, 0xd9, 0xee, 0xe5, 0x5d, 0x7b, 0x33, 
    0xc5, 0xdd, 0x75, 0x9c, 0x13, 0xac, 0xb8, 0xf1, 0xea, 0x8b, 0xed, 0xcd, 0x30, 0xf5, 0x3e, 0xb0, 
    0x8f, 0xfa, 0xac, 0x27, 0x2d, 0x97, 0xec, 0x61, 0x2e, 0x30, 0xe6, 0x59, 0xa8, 0xf9, 0x0c, 0x12, 
    0x66, 0x33, 0x6c, 0xcf, 0x74, 0x53, 0xff, 0xad, 0x97, 0xbd, 0x7f, 0xb8, 0xd4, 0xfc, 0x70, 0xf2, 
    0x91, 0x38, 0x0d, 0xf6, 0x29, 0x9c, 0x50, 0xf9, 0xca, 0x29, 0x09, 0xab, 0x1a, 0x6c, 0x88, 0x66, 
    0x20, 0xa5, 0xe2, 0xb5, 0x04, 0xab, 0x3f, 0x28, 0x39, 0x8e, 0xe2, 0x91, 0x66, 0xd0, 0xdf, 0xc3, 
    0x9e, 0x54, 0xfb, 0xe5, 0xb6, 0xa5, 0xbd, 0x85, 0xe1, 0x2d, 0x98, 0x3f, 0xcf, 0xc1, 0xc9, 0x07, 
    0x70, 0x07, 0xa8, 0x5f, 0xdf, 0x9a, 0xc0, 0xe3, 0xf6, 0xb9, 0x17, 0xd8, 0xe2, 0xa1, 0xa6, 0x71, 
    0xf6, 0xb9, 0x3f, 0x50, 0x2b, 0x3b, 0xe6, 0xbc, 0x17, 0x2c, 0x27, 0x89, 0xb6, 0x18, 0xca, 0xb6, 
    0xf4, 0xe8, 0x1c, 0x90, 0x1e, 0x32, 0x5a, 0x0f, 0x54, 0x94, 0x24, 0x2e, 0xf4, 0x72, 0x2c, 0xfd, 
    0xbe, 0xad, 0x55, 0x10, 0xea, 0x79, 0x2f, 0x1a, 0x75, 0x37, 0x4a, 0x69, 0xa4, 0xd2, 0x7f, 0x06, 
    0xfd, 0xde, 0x77, 0x5d, 0x37, 0x77, 0x28, 0x0a, 0xa1, 0x50, 0xfc, 0xa6, 0x11, 0x0d, 0x8e, 0x58, 
    0xd0, 0x80, 0x48, 0x9b, 0x25, 0xd2, 0x54, 0x40, 0xbd, 0x41, 0x95, 0xb3, 0xa3, 0x99, 0xbf, 0x9a, 
    0x64, 0x36, 0x3f, 0xfd, 0x30, 0x44, 0xd6, 0x3a, 0xa8, 0xc2, 0xc2, 0x7f, 0x9c, 0x6f, 0x82, 0x2e, 
    0x5e, 0x6a, 0x68, 0xb8, 0x36, 0x34, 0x68, 0x9d, 0x9b, 0xb6, 0xe6, 0xf4, 0x9f, 0xd6, 0x54, 0x52, 
    0xcf, 0xf0, 0x9e, 0x22, 0x00, 0x87, 0x1d, 0xd7, 0x64, 0xa9, 0x8d, 0xad, 0xe9, 0x8c, 0x4e, 0xec
};

// 启动 BLE 蓝牙配网
static void start_ble_provisioning(void) {
    if (s_provisioning_started) return;
    s_provisioning_started = true;
    
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    char service_name[16];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
    
    // 组装 SECURITY_2 专属结构体
    network_prov_security2_params_t sec2_params = {
        .salt = sec2_salt,
        .salt_len = sizeof(sec2_salt),
        .verifier = sec2_verifier,
        .verifier_len = sizeof(sec2_verifier)
    };

    // 传入配网管理器
    network_prov_security_t security = NETWORK_PROV_SECURITY_2;
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)&sec2_params, service_name, NULL));
    
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "📱 蓝牙配网已就绪！");
    ESP_LOGI(TAG, "请打开 Espressif 'ESP BLE Provisioning' 手机 App");
    ESP_LOGI(TAG, "🔍 搜索设备: %s", service_name);
    ESP_LOGI(TAG, "🔑 验证 PIN 码: %s", PROV_PIN);
    ESP_LOGI(TAG, "=================================================");
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册常规网络事件
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    // 注册配网生命周期事件
    esp_event_handler_instance_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler, NULL, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // ================= 🌟 核心修复：安全叠加 WPA3 配置 =================
    wifi_config_t wifi_config;
    // 1. 先获取 ESP 底层 NVS 中保存的配置（保留原有的 SSID 和 Password）
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    
    // 2. 在原有配置基础上，叠加 WPA3 与 PMF 的安全策略
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    // 3. 将安全配置写回底层
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // ====================================================================

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // 1. 初始化配网管理器，仅仅是为了检查是否已有凭据
    bool provisioned = false;
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    // 检查完毕后，立即注销管理器！
    network_prov_mgr_deinit(); 

    if (provisioned) {
        ESP_LOGI(TAG, "🔍 检测到已保存的 Wi-Fi 凭证，尝试直接连接 (最多尝试 %d 次)...", MAX_RETRY_WIFI);
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_LOGW(TAG, "⚠️ 未检测到 Wi-Fi 凭证，初次启动直接进入蓝牙配网模式！");
        ESP_ERROR_CHECK(esp_wifi_start());
        
        // 这里的调用现在是绝对安全的
        start_ble_provisioning();
    }
}

static void modem_net_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == MODEM_EVENT && event_id == MODEM_EVENT_NET_CHANGED) {
        // 从底层传递上来的原始 payload (如 "+CEREG: 1,1")
        const char* payload = (const char*)event_data;
        
        // 获取解析后的最新状态枚举
        modem_net_status_t current_status = modem_get_cellular_status();
        
        ESP_LOGI("APP_MAIN", "收到蜂窝网络状态改变事件！原始数据: %s, 当前状态枚举: %d", payload, current_status);
        
        // 状态 1 (本地网络) 和 5 (漫游网络) 都代表已经成功附着蜂窝网络
        if (current_status == MODEM_NET_STATUS_REGISTERED_HOME || current_status == MODEM_NET_STATUS_REGISTERED_ROAMING) {
            ESP_LOGI(TAG, "🟢 蜂窝网络已附着，熄灭 LED");
            gpio_set_level(LED_BUILTIN, LED_OFF);
        } else {
            ESP_LOGW(TAG, "🔴 蜂窝网络未附着或掉线，点亮 LED");
            gpio_set_level(LED_BUILTIN, LED_ON);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "📱 SMS网关启动中");
    ESP_LOGI(TAG, "=================================");

    // ==========================================
    // 初始化 LED GPIO
    // ==========================================
    gpio_reset_pin(LED_BUILTIN);
    // 设置为输出模式
    gpio_set_direction(LED_BUILTIN, GPIO_MODE_OUTPUT);
    // 初始状态：刚开机还未连接网络，常亮
    gpio_set_level(LED_BUILTIN, LED_ON); 

    // 初始化持久化配置 (NVS)
    config_manager_init(); // 注意：必须确保内部调用了 nvs_flash_init()
    config_load_all();
    log_service_init();

    // 初始化网络栈与配网逻辑
    wifi_init_sta();

    // 初始化推送服务组件
    push_service_init();

    // 启动短信处理引擎 (PDU 队列监听)
    sms_processor_init();
    // 启动来电处理组件
    call_processor_init();

    // 启动底层 Modem 驱动 (UART 中断)
    modem_driver_init();

    esp_event_handler_instance_register(
        MODEM_EVENT, 
        MODEM_EVENT_NET_CHANGED, 
        &modem_net_event_handler, 
        NULL, 
        NULL
    );
    
    char resp[128];
    int retry = 0;
    ESP_LOGI(TAG, "正在與 4G 模組同步波特率，請稍候...");
    
    while (modem_send_at_command("AT", resp, sizeof(resp), 1000) != ESP_OK && retry < 15) {
        ESP_LOGW(TAG, "等待模組就緒... (%d/15)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }

    if (retry >= 15) {
        ESP_LOGE(TAG, "🚨 無法與 4G 模組通訊！請檢查：");
        ESP_LOGE(TAG, "1. 模組電源是否足夠？");
        ESP_LOGE(TAG, "2. TXD(3) 和 RXD(4) 是否接反或有虛焊？");
    } else {
        ESP_LOGI(TAG, "✅ 波特率同步成功！模組已就緒。");
        
        modem_send_at_command("ATE0", resp, sizeof(resp), 2000);
        modem_send_at_command("AT+CMGF=0", resp, sizeof(resp), 2000); 
        modem_send_at_command("AT+CNMI=2,2,0,0,0", resp, sizeof(resp), 2000); 
        modem_send_at_command("AT+CLIP=1", resp, sizeof(resp), 2000); 

        if (strlen(g_app_config.plmn) == 0 || strcmp(g_app_config.plmn, "AUTO") == 0) {
            ESP_LOGI(TAG, "PLMN 未配置或 AUTO，使用自動註冊模式");
            modem_send_at_command("AT+COPS=0", resp, sizeof(resp), 10000);
        } else {
            char cops_cmd[64];
            snprintf(cops_cmd, sizeof(cops_cmd), "AT+COPS=1,2,\"%s\"", g_app_config.plmn);
            if (modem_send_at_command(cops_cmd, resp, sizeof(resp), 10000) != ESP_OK) {
                ESP_LOGW(TAG, "設定 PLMN(%s) 失敗，改回自動註冊", g_app_config.plmn);
                modem_send_at_command("AT+COPS=0", resp, sizeof(resp), 10000);
            }
        }

        if (strlen(g_app_config.smsc) == 0) {
            ESP_LOGI(TAG, "SMSC 未配置，使用默认短信中心号码");
        } else {
            ESP_LOGI(TAG, "設定 SMSC: %s", g_app_config.smsc);
            if (modem_set_smsc(g_app_config.smsc) != ESP_OK) {
                ESP_LOGW(TAG, "設定 SMSC 失敗");
            }
        }

        if (strlen(g_app_config.imei) == 0) {
            ESP_LOGI(TAG, "IMEI 未配置，保持模组默认 IMEI");
        } else {
            ESP_LOGW(TAG, "自訂 IMEI 在本固件版本中不支援寫入，保留本地設定供後續顯示使用");
        }
    }

    // 启动 Web 控制台
    web_server_start();

    // 启动定时任务管理器
    cron_tasker_init();

    ESP_LOGI(TAG, "🚀 系统所有核心服务已成功拉起，运行中...");

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}