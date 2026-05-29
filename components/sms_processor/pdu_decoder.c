#include "pdu_decoder.h"
#include "esp_log.h"
#include "log_service.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "PDU_DEC";

// ================= 基础工具函数 =================

// 将单个 16 进制字符转换为数值 (0-15)
static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 将 16 进制字符串转换为 Byte 数组 (🚀 高鲁棒性修复版)
static int hex_string_to_bytes(const char* hex_str, uint8_t* out_bytes, int max_len) {
    int byte_count = 0;
    int i = 0;

    // 跳过可能存在的开头空白字符或换行
    while (hex_str[i] == ' ' || hex_str[i] == '\r' || hex_str[i] == '\n') {
        i++;
    }

    while (hex_str[i] != '\0' && hex_str[i+1] != '\0' && byte_count < max_len) {
        int h = hex_char_to_int(hex_str[i]);
        int l = hex_char_to_int(hex_str[i+1]);
        
        // 核心修复：如果遇到非十六进制字符（比如遇到了模组附加的 \r\n 或者是 OK），
        // 直接终止转换并保留已成功解析的字节，绝不抛弃前面的数据！
        if (h < 0 || l < 0) {
            ESP_LOGD(TAG, "解析到非十六进制边界，已成功提取 %d bytes", byte_count);
            break; 
        }
        
        out_bytes[byte_count++] = (uint8_t)((h << 4) | l);
        i += 2;
    }
    
    return byte_count;
}

// ================= PDU 特定解析函数 =================

// 解析 PDU 中的号码 (半字节翻转，例如 683108130800F0 -> 8613800138000)
static int parse_phone_number(const uint8_t* data, int start_idx, char* out_num) {
    int num_len = data[start_idx]; // 号码数字的个数 (注意：不是字节数)
    if (num_len == 0) {
        strcpy(out_num, "Unknown");
        return start_idx + 2;
    }
    
    int type_of_address = data[start_idx + 1];
    int current_out_idx = 0;
    
    // 如果是国际号码 (Type 0x91)，前置加 '+'
    if (type_of_address == 0x91) {
        out_num[current_out_idx++] = '+';
    }
    
    // 实际存放号码的字节数：(数字个数 + 1) / 2
    int byte_len = (num_len + 1) / 2;
    int idx = start_idx + 2;
    
    for (int i = 0; i < byte_len; i++) {
        uint8_t b = data[idx + i];
        int low = b & 0x0F;
        int high = (b & 0xF0) >> 4;
        
        if (current_out_idx < 19) { // 保护缓冲不溢出
            out_num[current_out_idx++] = '0' + low;
        }
        // 如果长度是奇数，最后一个高位是 0xF，应当丢弃
        if (high != 0x0F && current_out_idx < 19) {
            out_num[current_out_idx++] = '0' + high;
        }
    }
    out_num[current_out_idx] = '\0';
    
    return start_idx + 2 + byte_len; // 返回号码解析结束的索引
}

// 解析 PDU 中的时间戳 (半字节翻转，格式 YYMMDDHHMMSS+TimeZone)
static int parse_timestamp(const uint8_t* data, int start_idx, char* out_time) {
    char raw[15];
    for (int i = 0; i < 7; i++) {
        uint8_t b = data[start_idx + i];
        raw[i*2] = '0' + (b & 0x0F);
        raw[i*2+1] = '0' + ((b & 0xF0) >> 4);
    }
    raw[14] = '\0';
    
    // 转换为 20YY-MM-DD HH:MM:SS
    snprintf(out_time, 32, "20%c%c-%c%c-%c%c %c%c:%c%c:%c%c", 
             raw[0], raw[1], // 年
             raw[2], raw[3], // 月
             raw[4], raw[5], // 日
             raw[6], raw[7], // 时
             raw[8], raw[9], // 分
             raw[10], raw[11]); // 秒
             
    return start_idx + 7;
}

// UCS2 (大端) 转换为 UTF-8 (支持中文)
static void ucs2_to_utf8(const uint8_t* ucs2_data, int byte_len, char* out_utf8) {
    int out_idx = 0;
    for (int i = 0; i < byte_len; i += 2) {
        // PDU 中的 UCS2 是大端模式 (Big-Endian)
        uint16_t wc = (ucs2_data[i] << 8) | ucs2_data[i+1];
        
        if (wc < 0x80) {
            out_utf8[out_idx++] = (char)wc;
        } else if (wc < 0x800) {
            out_utf8[out_idx++] = (char)(0xC0 | (wc >> 6));
            out_utf8[out_idx++] = (char)(0x80 | (wc & 0x3F));
        } else {
            out_utf8[out_idx++] = (char)(0xE0 | (wc >> 12));
            out_utf8[out_idx++] = (char)(0x80 | ((wc >> 6) & 0x3F));
            out_utf8[out_idx++] = (char)(0x80 | (wc & 0x3F));
        }
    }
    out_utf8[out_idx] = '\0';
}

// 7-bit GSM 默认字母表解码
static void gsm7bit_decode(const uint8_t* data, int septet_count, char* out_text) {
    int out_idx = 0;
    int bit_offset = 0;
    int byte_offset = 0;
    
    for (int i = 0; i < septet_count; i++) {
        uint8_t c = (data[byte_offset] >> bit_offset) & 0x7F;
        
        // 如果跨越了字节边界，把下一个字节的低位拼接过来
        if (bit_offset > 1) {
            c |= (data[byte_offset + 1] << (8 - bit_offset)) & 0x7F;
        }
        
        bit_offset += 7;
        if (bit_offset >= 8) {
            bit_offset -= 8;
            byte_offset++;
        }
        
        out_text[out_idx++] = (char)c;
    }
    out_text[out_idx] = '\0';
}

// ================= 主解码函数 =================

esp_err_t pdu_decode(const char* pdu_hex, decoded_sms_t* out_sms) {
    if (pdu_hex == NULL || out_sms == NULL) return ESP_ERR_INVALID_ARG;
    
    // 初始化输出结构
    memset(out_sms, 0, sizeof(decoded_sms_t));
    
    // 1. 将十六进制转换为字节数组 (假设 PDU 字符串长度不超过 512)
    uint8_t buf[256];
    int buf_len = hex_string_to_bytes(pdu_hex, buf, sizeof(buf));
    
    // 🚀 容错判断：只要提到了有效字节就继续，否则才退出
    if (buf_len <= 0) {
        ESP_LOGE(TAG, "PDU 转换失败：未提取到有效的十六进制数据");
        return ESP_FAIL;
    }
    
    int idx = 0;
    
    // 2. 解析 SMSC (短信中心号码) 长度
    int smsc_len = buf[idx++];
    idx += smsc_len; // 我们通常不关心接收端的 SMSC 信息，直接跳过
    
    if (idx >= buf_len) return ESP_FAIL;
    
    // 3. 解析 PDU Type (第一字节，包含各种标志位)
    uint8_t pdu_type = buf[idx++];
    
    // 检查 UDH (User Data Header) 标志位，这是判断长短信的关键
    bool has_udh = (pdu_type & 0x40) != 0; 
    
    // 4. 解析发件人号码 (Sender Address)
    idx = parse_phone_number(buf, idx, out_sms->sender);
    if (idx >= buf_len) return ESP_FAIL;
    
    // 5. 解析协议标识 (PID)
    uint8_t pid = buf[idx++];
    (void)pid; // 忽略
    
    // 6. 解析数据编码方案 (DCS)
    uint8_t dcs = buf[idx++];
    int encoding_type = 0; // 0:未知, 7:7-bit, 4:8-bit, 8:UCS2
    
    // dcs=0 (7-bit), dcs=4 (8-bit), dcs=8 (UCS2)
    if ((dcs & 0x0C) == 0x08 || dcs == 0x08) {
        encoding_type = 8; // UCS2 (Unicode，通常是中文)
    } else if ((dcs & 0x0C) == 0x04) {
        encoding_type = 4; // 8-bit (通常是数据报文)
    } else {
        encoding_type = 7; // 7-bit GSM 字母表 (纯英文)
    }
    
    // 7. 解析时间戳 (Service Center Time Stamp)
    idx = parse_timestamp(buf, idx, out_sms->timestamp);
    if (idx >= buf_len) return ESP_FAIL;
    
    // 8. 解析用户数据长度 (UDL)
    int user_data_len = buf[idx++];
    
    // 9. 解析用户数据 (User Data)
    const uint8_t* payload = &buf[idx];
    int payload_byte_len = buf_len - idx;
    
    if (payload_byte_len <= 0) {
        ESP_LOGW(TAG, "短信内容为空");
        return ESP_OK;
    }
    
    int content_start = 0;
    
    // ================= 处理 UDH (判断是否是长短信) =================
    if (has_udh && payload_byte_len > 0) {
        int udh_len = payload[0];
        
        // 查找 UDH 中的 Concatenated SMS (长短信) 标识 (IEI 0x00 或 0x08)
        int i = 1;
        while (i <= udh_len && i < payload_byte_len) {
            uint8_t iei = payload[i];
            uint8_t iel = payload[i+1];
            
            if (iei == 0x00 && iel == 3) { // 8-bit reference number
                out_sms->is_concat = true;
                out_sms->ref_number = payload[i+2];
                out_sms->total_parts = payload[i+3];
                out_sms->part_number = payload[i+4];
                break;
            } else if (iei == 0x08 && iel == 4) { // 16-bit reference number
                out_sms->is_concat = true;
                out_sms->ref_number = (payload[i+2] << 8) | payload[i+3];
                out_sms->total_parts = payload[i+4];
                out_sms->part_number = payload[i+5];
                break;
            }
            i += (2 + iel); // 跳过当前 Info Element
        }
        
        content_start = udh_len + 1; // 真正的短信文本要在跳过 UDH 之后
    }
    
    // ================= 开始解码真正的文本内容 =================
    if (encoding_type == 8) {
        // UCS2 解码 (大部分中文短信)
        int text_byte_len = payload_byte_len - content_start;
        if (text_byte_len > 0) {
            ucs2_to_utf8(&payload[content_start], text_byte_len, out_sms->text);
        }
        
    } else if (encoding_type == 7) {
        // 7-bit 解码
        int septet_count = user_data_len;
        if (has_udh) {
            int udh_bytes = payload[0] + 1; 
            int udh_septets = (udh_bytes * 8 + 6) / 7; // 向上取整
            septet_count -= udh_septets;
            gsm7bit_decode(&payload[content_start], septet_count, out_sms->text);
        } else {
            gsm7bit_decode(&payload[content_start], septet_count, out_sms->text);
        }
        
    } else {
        // 8-bit 数据，强行当作 ASCII 拷贝
        ESP_LOGW(TAG, "收到 8-bit 数据短信，尝试直接提取");
        int len = payload_byte_len - content_start;
        if (len >= sizeof(out_sms->text)) len = sizeof(out_sms->text) - 1;
        if (len > 0) {
            memcpy(out_sms->text, &payload[content_start], len);
            out_sms->text[len] = '\0';
        }
    }
    
    return ESP_OK;
}