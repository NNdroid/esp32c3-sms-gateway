#ifndef SMS_PROCESSOR_H
#define SMS_PROCESSOR_H

#include <stdbool.h>
#include "esp_err.h"

// 供其他模块 (比如 modem_driver) 调用的接口，将收到的 PDU 字符串压入队列
esp_err_t sms_processor_enqueue_pdu(const char* pdu_hex_str);

// 初始化短信处理任务 (队列创建与 Task 启动)
void sms_processor_init(void);

// 是否为管理员手机号
bool sms_processor_is_admin_phone(const char* sender);
// 是否命中黑名单
bool sms_processor_is_blacklisted(const char* sender);

#endif // SMS_PROCESSOR_H