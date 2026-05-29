#ifndef PDU_DECODER_H
#define PDU_DECODER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ================= 解码后的短信结构体 =================
typedef struct {
    char sender[20];            // 发件人号码 (例如 +8613800138000)
    char timestamp[32];         // 时间戳 (20YY-MM-DD HH:MM:SS)
    char text[160 * 3];         // 完整的 UTF-8 文本内容 (预留最大空间)
    
    // 长短信 (Concat SMS) 专用字段
    bool is_concat;             // 是否是长短信分片
    int ref_number;             // 长短信唯一标识码 (Reference Number)
    int total_parts;            // 总分片数
    int part_number;            // 当前是第几个分片 (1-based)
} decoded_sms_t;

/**
 * @brief 将 16 进制字符串格式的 PDU 解码为人类可读的结构体
 * * @param pdu_hex 原始的 16 进制 PDU 字符串
 * @param out_sms 用于存放解码结果的结构体指针
 * @return esp_err_t ESP_OK 表示解码成功
 */
esp_err_t pdu_decode(const char* pdu_hex, decoded_sms_t* out_sms);

#endif // PDU_DECODER_H