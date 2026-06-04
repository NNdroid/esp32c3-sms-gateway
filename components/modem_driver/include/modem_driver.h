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
/**
 * @brief Modem 事件 ID 枚举
 * @note 这些事件 ID 用于区分不同类型的 Modem 事件，例如网络状态变化、GNSS 位置更新、短信接收等。你可以根据需要在 modem_event_task 的 UART 解析逻辑中解析 AT 指令的响应或 URC 上报，并在适当的时候调用 esp_event_post 将事件发布到系统中，供其他组件订阅和处理。
 */
enum {
    MODEM_EVENT_SMS_RECEIVED, // 收到新短信
    MODEM_EVENT_CALL_RINGING, // 有来电
    MODEM_EVENT_NET_CHANGED,  // 网络状态改变
    MODEM_EVENT_PING_REPORT,   // 异步 Ping 结果
    MODEM_EVENT_GNSS_UPDATED,   // GNSS 位置更新
};
/**
 * @brief GNSS 位置数据结构
 * @note 该结构体用于存储从模组 GNSS 功能获取到的位置信息，包含时间、日期、经纬度、速度、航向、海拔、卫星数量、HDOP 等常用字段，以及一个 is_valid 字段用于指示当前数据是否有效（例如是否有足够的卫星信号进行定位）。请确保在调用 modem_driver_init() 完成初始化后再调用相关函数获取 GNSS 位置，以确保获取到正确的结果。
 */
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
 * @param enable true 开启 GNSS，false 关闭 GNSS
 * @return ESP_OK 成功，其他错误码表示失败（例如模组不支持 GNSS 功能）
 * @note 该函数会发送 AT 指令控制模组的 GNSS 功能，只有当模组真正执行成功后才会更新内部状态。请确保在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
 */
esp_err_t modem_set_gnss_state(bool enable);
/**
 * @brief 获取当前最新的 GNSS 位置 (直接读取后台缓存，非阻塞)
 * @param out_loc 输出参数，返回当前的 GNSS 位置数据
 * @return true 成功获取到有效位置，false 没有有效位置数据（例如卫星信号弱或未定位）
 * @note 该函数返回的数据是后台 UART 解析任务中通过解析 GNSS 相关 URC 上报时更新的，属于动态信息。请确保在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
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
 * @return modem_net_status_t 当前的蜂窝网络注册状态，具体含义请参考 modem_net_status_t 枚举定义
 * @note 该函数返回的状态是在后台 UART 解析任务中通过解析 +CEREG 或 +CREG URC 上报时更新的，属于动态信息。请确保在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
 */
modem_net_status_t modem_get_cellular_status(void);
/**
 * @brief 获取模组型号
 * @return const char* 模组型号字符串 (如果未获取到则返回 "UNKNOWN")
 * @note 该函数返回的模组型号是在初始化阶段通过发送 AT 指令查询模组型号后获取的，属于静态信息。请在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
 */
const char* modem_get_model(void);
/**
 * @brief 获取模组详细固件版本 (例如 "ML307A-GSLN-MTSH1S00")
 * @return const char* 模组固件版本字符串 (如果未获取到则返回 "UNKNOWN")
 * @note 该函数返回的固件版本是在初始化阶段通过发送 AT 指令查询模组型号后获取的，属于静态信息。请在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
 */
const char* modem_get_fw_version(void);
/**
 * @brief 检查当前模组是否支持 GNSS 功能
 * @return true: 支持 (例如型号为 ML307A-GSLN), false: 不支持
 * @note 该函数的返回值是在初始化阶段通过发送 AT 指令查询模组型号后确定的，属于静态信息。请在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
 */
bool modem_has_gnss(void);
// ================= 4G 数据网络控制接口 =================
/**
 * @brief 允许或禁止模组使用 4G 数据网络
 * @param enable true 允许，false 禁止
 * @return ESP_OK 成功，其他错误码表示失败
 * @note 该接口仅在逻辑层控制是否允许模组使用数据网络，实际的网络连接状态还需要通过 modem_get_cellular_status() 来查询。禁止数据网络后，模组仍然可以进行短信和语音通话等非数据业务，但无法建立 PDP 上下文进行数据通信。
 */
esp_err_t toggle_modem_data_network(bool enable);
/**
 * @brief 查询当前是否允许模组使用 4G 数据网络
 * @return true 允许，false 禁止
 * @note 该函数返回的是逻辑层的允许状态，并不代表当前模组实际的网络连接状态。请结合 modem_get_cellular_status() 的结果来综合判断模组的网络使用情况。
 */
bool is_modem_data_allowed(void);
/**
 * @brief AT 指令异步执行回调函数指针类型
 * @param result 指令执行结果，ESP_OK 成功，其他错误码表示失败
 * @param response 指令响应字符串，可能是 OK、ERROR 或其他返回数据
 * @param user_ctx 用户上下文指针，调用 modem_enqueue_at_command_async 时传入，回调时原样传回
 * @note 该回调函数会在后台 Worker 任务中被调用，必须尽快返回，不能执行耗时操作。建议将响应数据复制到用户上下文中，并通过消息队列或事件组通知其他任务处理。
 */
typedef void (*modem_at_cb_t)(esp_err_t result, const char* response, void* user_ctx);
/**
 * @brief URC (Unsolicited Result Code) 事件处理函数指针类型
 * @param event_id 事件 ID (例如 MODEM_EVENT_SMS_RECEIVED)
 * @param payload 原始事件数据字符串 (例如 "+CMT: ...")
 * @param user_ctx 用户上下文指针，注册 URC 处理函数时传入，事件发生时原样传回
 * @note URC 处理函数会在后台 UART 解析任务中被调用，必须尽快返回，不能执行耗时操作。建议将事件数据复制到用户上下文中，并通过消息队列或事件组通知其他任务处理。
 */
typedef void (*modem_urc_handler_t)(int event_id, const char* payload, void* user_ctx);
// ================= API 声明 =================
/**
 * @brief 初始化 Modem 驱动，配置 UART 和相关资源
 * @return ESP_OK 初始化成功，其他错误码表示失败
 * @note 该函数会初始化 UART 驱动、创建必要的 FreeRTOS 资源，并启动后台任务。它还会尝试与模组进行握手并查询型号信息，以确保模组正常工作。请在系统启动时调用该函数，并确保在调用其他 Modem API 之前完成初始化。
 */
esp_err_t modem_driver_init(void);
/**
 * @brief 同步发送 AT 指令并等待响应
 * @param cmd 要发送的 AT 指令字符串
 * @param out_resp 输出参数，接收响应字符串
 * @param max_len 输出缓冲区的最大长度
 * @param timeout_ms 等待响应的超时时间，单位毫秒
 * @return ESP_OK 收到 OK 响应，ESP_ERR_TIMEOUT 等待超时，ESP_FAIL 收到 ERROR 响应，其他错误码表示发送失败
 * @note 该函数会阻塞当前线程直到收到 OK、ERROR 或超时，适用于需要立即获取指令结果的场景。请注意不要在高优先级任务或中断中调用该函数，以免造成系统卡顿。
 */
esp_err_t modem_send_at_command(const char* cmd, char* out_resp, size_t max_len, uint32_t timeout_ms);
/**
 * @brief 异步执行 AT 指令并通过回调返回结果
 * @param cmd 要执行的 AT 指令字符串
 * @param timeout_ms 等待响应的超时时间，单位毫秒
 * @param cb 回调函数指针，指令执行完成后会调用该函数并传入结果
 * @param user_ctx 用户上下文指针，会原样传回给回调函数，方便用户携带额外信息
 * @return ESP_OK 成功将指令加入异步队列，ESP_ERR_INVALID_ARG 参数错误，ESP_ERR_NO_MEM 内存不足，ESP_ERR_TIMEOUT 队列满无法加入
 * @note 该函数会将 AT 指令封装成一个异步请求结构体，并尝试将其加入到后台 Worker 任务的队列中执行。指令执行完成后，Worker 任务会调用用户提供的回调函数并传入执行结果和响应字符串。请确保回调函数能够正确处理可能的超时或错误情况。
 */
esp_err_t modem_enqueue_at_command_async(const char* cmd, uint32_t timeout_ms, modem_at_cb_t cb, void* user_ctx);
/**
 * @brief 等待 AT 指令提示符 ">"，通常用于发送 AT+CMGS 后等待输入短信内容
 * @param timeout_ms 等待超时时间，单位毫秒
 * @return ESP_OK 收到提示符，ESP_ERR_TIMEOUT 等待超时，其他错误码表示等待失败
 * @note 该函数会阻塞当前线程直到收到提示符或超时，适用于需要等待用户输入的 AT 指令场景。请确保在调用该函数前已经正确发送了相关 AT 指令，并且模组处于等待输入的状态。
 */
esp_err_t modem_wait_for_prompt(uint32_t timeout_ms);
/**
 * @brief 直接发送原始数据到 UART（适用于短信内容发送等特殊场景）
 * @param data 要发送的字符串数据，必须以 null 字符结尾
 * @note 该函数不会添加任何前缀或后缀，也不会等待响应，适合在发送 AT+CMGS 后等待提示符时使用。请确保调用该函数时已经正确处理了 AT 指令的发送和响应逻辑。
 */
void modem_send_raw_data(const char* data);
/**
 * @brief 获取当前 SMSC 地址
 * @param smsc 输出参数，接收 SMSC 地址字符串
 * @param max_len 输出缓冲区的最大长度
 * @return ESP_OK 获取成功，ESP_ERR_TIMEOUT 获取超时，其他错误码表示获取失败
 * @note SMSC 地址通常由运营商提供，格式类似于 "+8613800138000"。正确的 SMSC 地址对于短信发送功能至关重要，如果不确定请咨询你的 SIM 卡运营商。
 * 该函数会发送 AT+CSCA? 指令查询当前 SMSC 地址，并解析响应中的地址字符串。获取到的 SMSC 地址会被复制到调用者提供的缓冲区中，并以 null 字符结尾。
 */
esp_err_t modem_get_smsc(char* smsc, size_t max_len);
/**
 * @brief 设置 SMSC 地址
 * @param smsc SMSC 地址字符串，如果传入空字符串则恢复默认 SMSC
 * @return ESP_OK 设置成功，ESP_ERR_TIMEOUT 设置超时，其他错误码表示设置失败
 * @note SMSC 地址通常由运营商提供，格式类似于 "+8613800138000"。正确的 SMSC 地址对于短信发送功能至关重要，如果不确定请咨询你的 SIM 卡运营商。
 */
esp_err_t modem_set_smsc(const char* smsc);
/**
 * @brief 通过PING指令测试时延
 * @param target 目标地址字符串（IP 或域名）
 * @param count 发送的 Ping 包数量
 * @param timeout_s 每个 Ping 包的超时时间，单位秒
 * @param out_success_count 输出参数，接收成功的 Ping 包数量
 * @param out_avg_rtt 输出参数，接收平均往返时延（单位毫秒）
 * @param details 输出参数，接收详细结果字符串（每行格式为 "Reply from <IP>: time=<RTT>ms"）
 * @param details_len 输出缓冲区的最大长度
 * @return ESP_OK 测试完成，ESP_ERR_TIMEOUT 测试超时，其他错误码表示测试失败
 * @note 该函数会发送 AT+MPING 指令启动 Ping 测试，并通过注册 URC 事件回调的方式异步接收每个 Ping 包的结果。函数会等待所有 Ping 包完成或超时后返回，并将统计结果通过输出参数返回给调用者。
 */
esp_err_t modem_ping(const char* target, int count, int timeout_s, int* out_success_count, int* out_avg_rtt, char* details, size_t details_len);
/**
 * @brief 发送文本短信 (AT+CMGS)
 * @param phone 目标电话号码字符串
 * @param content 短信内容字符串
 * @param timeout_ms 发送超时时间，单位毫秒
 * @return ESP_OK 发送成功，ESP_ERR_TIMEOUT 发送超时，其他错误码表示发送失败
 * @note 该函数会自动切换到文本模式发送短信，发送完成后会恢复到 PDU 模式。发送过程中会等待 AT 提示符，适合发送较长文本且需要确认发送结果的场景。
 */
esp_err_t modem_send_sms_text(const char* phone, const char* content, uint32_t timeout_ms);
/**
 * @brief 读取指定短信槽位的 PDU 数据
 * @param index 短信槽位索引 (通常从 1 开始)
 * @param out_pdu 输出参数，接收 PDU 数据字符串
 * @param max_len 输出缓冲区的最大长度
 * @return ESP_OK 读取成功，ESP_ERR_TIMEOUT 读取超时，其他错误码表示读取失败
 * @note 读取到的 PDU 数据是十六进制字符串格式，需要调用者自行解析成短信内容。读取成功后对应槽位的短信仍然保留，调用者可以根据需要决定是否删除。
 */
esp_err_t modem_read_sms_pdu(int index, char* out_pdu, size_t max_len);
/**
 * @brief 删除指定短信槽位的短信
 * @param index 短信槽位索引 (通常从 1 开始)
 * @return ESP_OK 删除成功，ESP_ERR_TIMEOUT 删除超时，其他错误码表示删除失败
 * @note 删除短信后对应槽位会被清空，后续可以被新短信占用。
 */
esp_err_t modem_delete_sms(int index);
/**
 * @brief 获取所有已存短信的槽位索引
 * @param out_indices 输出参数，接收短信槽位索引数组
 * @param max_count 输出数组的最大容量
 * @param out_count 输出参数，接收实际的短信数量
 * @return ESP_OK 获取成功，ESP_ERR_TIMEOUT 获取超时，其他错误码表示获取失败
 * @note 该函数会发送 AT+CMGL=4 指令查询所有短信，并解析响应中的短信槽位索引。获取到的索引会被复制到调用者提供的数组中，并以 null 字符结尾。请确保调用该函数前已经正确初始化 Modem 驱动，并且模组中确实有短信存储。
 */
esp_err_t modem_get_all_sms_indices(int* out_indices, int max_count, int* out_count);
/**
 * @brief 注册 URC 事件回调
 * @param handler 事件处理函数
 * @param user_ctx 传递给事件回调的上下文
 * @return ESP_OK 注册成功，ESP_ERR_INVALID_ARG 参数错误，ESP_ERR_NO_MEM 内存不足，ESP_ERR_TIMEOUT 获取超时
 * @note 注册的事件处理函数会在后台任务中被调用，接收来自模组的 URC 事件。请确保事件处理函数能够正确处理可能的并发调用，并且在不再需要接收 URC 事件时及时调用 modem_unregister_urc_handler 进行注销。
 */
esp_err_t modem_register_urc_handler(modem_urc_handler_t handler, void* user_ctx);
/**
 * @brief 注销 URC 事件回调
 * @param handler 事件处理函数
 * @param user_ctx 传递给事件回调的上下文
 * @return ESP_OK 注销成功，ESP_ERR_INVALID_ARG 参数错误，ESP_ERR_TIMEOUT 获取超时，ESP_ERR_NOT_FOUND 未找到对应的处理函数
 * @note 注销时需要提供与注册时完全相同的 handler 和 user_ctx 参数组合才能成功注销，否则会返回 ESP_ERR_NOT_FOUND 错误。请确保在不再需要接收 URC 事件时及时调用该函数进行注销，以避免资源泄漏或意外事件处理。
 */
esp_err_t modem_unregister_urc_handler(modem_urc_handler_t handler, void* user_ctx);

#endif // MODEM_DRIVER_H