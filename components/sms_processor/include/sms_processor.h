#ifndef SMS_PROCESSOR_H
#define SMS_PROCESSOR_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief 初始化短信处理引擎
 * @note 必须在 modem_driver_init 之前调用，以确保 URC 事件队列和任务准备就绪
 */
void sms_processor_init(void);

/**
 * @brief 检查号码是否为管理员手机号
 */
bool sms_processor_is_admin_phone(const char* sender);

/**
 * @brief 检查号码是否命中黑名单
 */
bool sms_processor_is_blacklisted(const char* sender);

/**
 * @brief 将原始 PDU 字符串强制压入事件队列 (直通模式兼容接口)
 * @note 在完全基于事件回调的新架构中，此接口可仅用作内部或特殊的手动测试调用
 */
esp_err_t sms_processor_enqueue_pdu(const char* pdu_hex_str);

/**
 * @brief 重试之前因网络问题未能成功推送的消息
 * @note 该函数会扫描之前未成功推送的消息记录，并尝试重新推送。请在网络状态发生变化时调用此函数，以确保尽快将未推送的消息发送出去。
 */
void sms_processor_retry_failed_pushes(void);

#endif // SMS_PROCESSOR_H