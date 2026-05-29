#ifndef MODEM_DRIVER_H
#define MODEM_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

// 硬件引脚定义 (ESP32-C3)
#define MODEM_TXD_PIN  3
#define MODEM_RXD_PIN  4
#define MODEM_EN_PIN   5

#define MODEM_UART_NUM UART_NUM_1
#define UART_BUF_SIZE  2048

// 声明全局 Modem 事件基底
ESP_EVENT_DECLARE_BASE(MODEM_EVENT);

// 事件 ID 枚举
enum {
    MODEM_EVENT_SMS_RECEIVED, // 收到新短信
    MODEM_EVENT_CALL_RINGING, // 有来电
    MODEM_EVENT_NET_CHANGED,  // 网络状态改变
    MODEM_EVENT_PING_REPORT,   // 异步 Ping 结果
    MODEM_EVENT_GNSS_UPDATED,   // GNSS 位置更新
};

// GNSS 位置结构体
typedef struct {
    char time[16];
    char date[16];
    double latitude;
    double longitude;
    float speed;
    float heading;
    float altitude;
    int satellites;
    float hdop;
    bool is_valid;
} gnss_location_t;

/**
 * @brief 开启或关闭 GNSS (对应 AT+MGNSS=1/0)
 */
esp_err_t modem_set_gnss_state(bool enable);

/**
 * @brief 获取当前最新的 GNSS 位置 (直接读取后台缓存，非阻塞)
 */
bool modem_get_location(gnss_location_t* out_loc);

// 蜂窝网络注册状态枚举 (参考 3GPP 27.007 标准)
typedef enum {
    MODEM_NET_STATUS_UNKNOWN = 0,           // 未知状态
    MODEM_NET_STATUS_NOT_REGISTERED = 1,    // 未注册，且没有在搜索网络
    MODEM_NET_STATUS_SEARCHING = 2,         // 未注册，但正在搜索网络
    MODEM_NET_STATUS_REGISTERED_HOME = 3,   // 已注册 (本地网络)
    MODEM_NET_STATUS_DENIED = 4,            // 注册被拒绝
    MODEM_NET_STATUS_REGISTERED_ROAMING = 5 // 已注册 (漫游网络)
} modem_net_status_t;

/**
 * @brief 获取当前全局的 SIM 卡附着状态
 */
modem_net_status_t modem_get_cellular_status(void);

/**
 * @brief 获取模组型号
 * * @return const char* 模组型号字符串 (如果未获取到则返回 "UNKNOWN")
 */
const char* modem_get_model(void);

/**
 * @brief 获取模组详细固件版本 (例如 "ML307A-GSLN-MTSH1S00")
 */
const char* modem_get_fw_version(void);

/**
 * @brief 检查当前模组是否支持 GNSS 功能
 * @return true: 支持 (例如型号为 ML307A-GSLN), false: 不支持
 */
bool modem_has_gnss(void);

// ================= 4G 数据网络控制接口 =================
/**
 * @brief 允许或禁止模组使用 4G 数据网络
 * @param enable true 允许，false 禁止
 */
esp_err_t toggle_modem_data_network(bool enable);
/**
 * @brief 查询当前是否允许模组使用 4G 数据网络
 * @return true 允许，false 禁止
 */
bool is_modem_data_allowed(void);

// 异步任务回调函数指针类型
typedef void (*modem_at_cb_t)(esp_err_t result, const char* response, void* user_ctx);

typedef void (*modem_urc_handler_t)(int event_id, const char* payload, void* user_ctx);

// ================= API 声明 =================

// 初始化 Modem 驱动并启动后台监听 Task
esp_err_t modem_driver_init(void);

// 线程安全的 AT 指令发送与阻塞等待响应 (同步)
esp_err_t modem_send_at_command(const char* cmd, char* out_resp, size_t max_len, uint32_t timeout_ms);

// 异步发送 AT 指令 (基于回调，非阻塞)
esp_err_t modem_enqueue_at_command_async(const char* cmd, uint32_t timeout_ms, modem_at_cb_t cb, void* user_ctx);

// 针对发短信场景的特殊封装
esp_err_t modem_wait_for_prompt(uint32_t timeout_ms);
void modem_send_raw_data(const char* data);

// 常用功能封装
esp_err_t modem_get_smsc(char* smsc, size_t max_len);
esp_err_t modem_set_smsc(const char* smsc);
esp_err_t modem_ping(const char* target, int count, int timeout_s, int* out_success_count, int* out_avg_rtt, char* details, size_t details_len);
esp_err_t modem_send_sms_text(const char* phone, const char* content, uint32_t timeout_ms);

/**
 * @brief 注册 URC 事件回调
 * @param handler 事件处理函数
 * @param user_ctx 传递给事件回调的上下文
 */
esp_err_t modem_register_urc_handler(modem_urc_handler_t handler, void* user_ctx);

/**
 * @brief 注销 URC 事件回调
 */
esp_err_t modem_unregister_urc_handler(modem_urc_handler_t handler, void* user_ctx);

#endif // MODEM_DRIVER_H