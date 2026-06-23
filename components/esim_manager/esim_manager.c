/*
 * esim_manager.c — eSIM (eUICC) Profile 管理模組 (Hardened & Verified)
 *
 * 專為解決 9esim 及各類非標準 DIY 白卡而生：
 * 嚴格實施三級降級選通機制 (Standard -> No Refresh -> LPAC Nested)。
 */

#include "esim_manager.h"
#include "modem_driver.h"
#include "log_service.h"
#include "config_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

static const char *TAG = "ESIM_MGR";

#define MAX_CACHED_PROFILES 10
static esim_profile_t s_cached_profiles[MAX_CACHED_PROFILES];
static int s_cached_profile_count = 0;

// ==================== 常量 ====================

static const uint8_t ESIM_ISD_R_AID[] = {
    0xA0, 0x00, 0x00, 0x05, 0x59, 0x10, 0x10, 0xFF,
    0xFF, 0xFF, 0xFF, 0x89, 0x00, 0x00, 0x01, 0x00
};

#define ESIM_AID_ALT1 "A0000001514149530000000000000000"
#define ESIM_AID_ALT2 "A0000005591010FFFFFFFF8900000000"
#define ESIM_AID_ALT3 "A0000005591010FFFFFFFF8900000101"

#define APDU_MAX_RESP 4096

// ==================== 全局狀態 ====================

static bool s_esim_supported = false;
static char s_last_error[128] = "";

// ==================== 輔助函數 ====================

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

const char *esim_manager_get_last_error(void) {
    return s_last_error;
}

bool esim_manager_is_supported(void) {
    return s_esim_supported;
}

static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool is_hex_string(const char *str) {
    size_t len = strlen(str);
    if (len == 0 || (len % 2) != 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (!is_hex_char(str[i])) return false;
    }
    return true;
}

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    return c - 'a' + 10;
}

static void bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size) {
    static const char digits[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_size; i++) {
        out[pos++] = digits[data[i] >> 4];
        out[pos++] = digits[data[i] & 0x0F];
    }
    out[pos] = '\0';
}

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_size) {
    size_t len = strlen(hex) / 2;
    if (len > out_size) len = out_size;
    for (size_t i = 0; i < len; i++) {
        out[i] = (hex_nibble(hex[i * 2]) << 4) | hex_nibble(hex[i * 2 + 1]);
    }
    return len;
}

// 純數字 ICCID 轉 電話BCD（強制對齊 esim.cpp 鎖死 10 字節長度並補 FF）
static bool iccid_to_bcd(const char *iccid, uint8_t *bcd, size_t bcd_size, size_t *out_len) {
    if (bcd_size < 10) return false;
    size_t digits = strlen(iccid);
    if (digits == 0 || digits > 20) return false;
    for (size_t i = 0; i < digits; i++) {
        if (!isdigit((unsigned char)iccid[i])) return false;
    }

    memset(bcd, 0xFF, 10);
    for (size_t i = 0; i < digits; i += 2) {
        uint8_t lo = iccid[i] - '0';
        uint8_t hi = (i + 1 < digits) ? (iccid[i + 1] - '0') : 0x0F;
        bcd[i / 2] = (hi << 4) | lo;
    }
    *out_len = 10;
    return true;
}

static void bcd_to_iccid(const uint8_t *bcd, size_t bcd_len, char *out, size_t out_size) {
    size_t n = 0;
    for (size_t i = 0; i < bcd_len && n + 1 < out_size; i++) {
        uint8_t lo = bcd[i] & 0x0F;
        uint8_t hi = (bcd[i] >> 4) & 0x0F;
        if (lo <= 9) out[n++] = '0' + lo;
        if (hi <= 9 && n + 1 < out_size) out[n++] = '0' + hi;
    }
    out[n] = '\0';
}

// ==================== TLV 解析/生成 ====================

typedef struct {
    uint32_t tag;
    const uint8_t *value;
    size_t length;
    size_t next_offset;
} tlv_node_t;

static bool read_tlv(const uint8_t *data, size_t len, size_t offset, tlv_node_t *node) {
    if (!node || offset >= len) return false;
    size_t pos = offset;
    uint32_t tag = data[pos++];
    if ((tag & 0x1F) == 0x1F) {
        do {
            if (pos >= len) return false;
            tag = (tag << 8) | data[pos];
        } while ((data[pos++] & 0x80) != 0);
    }
    if (pos >= len) return false;
    uint8_t len_byte = data[pos++];
    size_t value_len = 0;
    if ((len_byte & 0x80) == 0) {
        value_len = len_byte;
    } else {
        uint8_t len_bytes = len_byte & 0x7F;
        if (len_bytes == 0 || len_bytes > sizeof(size_t) || pos + len_bytes > len) return false;
        for (uint8_t i = 0; i < len_bytes; i++) {
            value_len = (value_len << 8) | data[pos++];
        }
    }
    if (pos + value_len > len) return false;
    node->tag = tag;
    node->value = data + pos;
    node->length = value_len;
    node->next_offset = pos + value_len;
    return true;
}

static bool find_child_tag(const uint8_t *data, size_t len, uint32_t tag, tlv_node_t *found) {
    size_t pos = 0;
    tlv_node_t node;
    while (read_tlv(data, len, pos, &node)) {
        if (node.tag == tag) {
            if (found) *found = node;
            return true;
        }
        pos = node.next_offset;
    }
    return false;
}

static void append_tag(uint8_t *out, size_t *pos, uint32_t tag) {
    if (tag > 0xFFFF) out[(*pos)++] = (uint8_t)(tag >> 16);
    if (tag > 0xFF)   out[(*pos)++] = (uint8_t)(tag >> 8);
    out[(*pos)++] = (uint8_t)(tag & 0xFF);
}

static void append_length(uint8_t *out, size_t *pos, size_t len) {
    if (len < 0x80) {
        out[(*pos)++] = (uint8_t)len;
    } else if (len <= 0xFF) {
        out[(*pos)++] = 0x81;
        out[(*pos)++] = (uint8_t)len;
    } else {
        out[(*pos)++] = 0x82;
        out[(*pos)++] = (uint8_t)(len >> 8);
        out[(*pos)++] = (uint8_t)(len & 0xFF);
    }
}

static void append_tlv(uint8_t *out, size_t *pos, uint32_t tag, const uint8_t *value, size_t len) {
    append_tag(out, pos, tag);
    append_length(out, pos, len);
    if (len > 0) {
        memcpy(out + *pos, value, len);
        *pos += len;
    }
}

// ==================== 邏輯通道管理 ====================

static esp_err_t open_channel(char *channel, size_t channel_buf_size) {
    char resp[256] = {0};
    esp_err_t err = ESP_FAIL;

    char aid_hex[64];
    bytes_to_hex(ESIM_ISD_R_AID, sizeof(ESIM_ISD_R_AID), aid_hex, sizeof(aid_hex));

    {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "AT+CCHO=\"%s\"", aid_hex);

        err = modem_send_at_command(cmd, resp, sizeof(resp), 10000);
        if (err == ESP_OK) goto parse_channel;

        if (strstr(resp, "ERROR: 20") || strstr(resp, "CME ERROR")) {
            ESP_LOGW(TAG, "邏輯通道滿，嘗試主動清理...");
            for (int i = 1; i <= 8; i++) {
                char close_cmd[32];
                snprintf(close_cmd, sizeof(close_cmd), "AT+CCHC=%d", i);
                modem_send_at_command(close_cmd, NULL, 0, 1000);
            }
            memset(resp, 0, sizeof(resp));
            err = modem_send_at_command(cmd, resp, sizeof(resp), 10000);
            if (err == ESP_OK) goto parse_channel;
        }
    }

    {
        const char *alt_aids[] = {ESIM_AID_ALT1, ESIM_AID_ALT2, ESIM_AID_ALT3, NULL};
        for (int i = 0; alt_aids[i] != NULL; i++) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+CCHO=\"%s\"", alt_aids[i]);
            memset(resp, 0, sizeof(resp));
            err = modem_send_at_command(cmd, resp, sizeof(resp), 10000);
            if (err == ESP_OK) goto parse_channel;
        }
    }

    set_error("打開 ISD-R 通道失敗: %s", resp);
    return ESP_FAIL;

parse_channel:
    char *p = strstr(resp, "+CCHO:");
    int ch = 0;
    if (p) {
        p += 6;
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        ch = atoi(p);
    } else {
        p = resp;
        while (*p) {
            if (*p >= '1' && *p <= '9') {
                ch = atoi(p);
                break;
            }
            p++;
        }
    }

    if (ch <= 0 || ch > 20) {
        set_error("獲取到的邏輯通道號無效: %d", ch);
        return ESP_FAIL;
    }

    snprintf(channel, channel_buf_size, "%d", ch);
    return ESP_OK;
}

static void close_channel(const char *channel) {
    if (!channel || channel[0] == '\0') return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CCHC=%s", channel);
    modem_send_at_command(cmd, NULL, 0, 2000);
}

// ==================== APDU 傳輸 ====================

static uint8_t class_byte_for_channel(const char *channel) {
    int ch = atoi(channel);
    if (ch <= 0 || ch > 19) return 0x80;
    if (ch < 4) return 0x80 | ch;
    return 0x80 | 0x40 | (ch - 4);
}

static esp_err_t transmit_apdu(const char *channel, const uint8_t *tx, size_t tx_len, uint8_t **rx, size_t *rx_len) {
    *rx = NULL;
    *rx_len = 0;

    char tx_hex[512];
    bytes_to_hex(tx, tx_len, tx_hex, sizeof(tx_hex));

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "AT+CGLA=%s,%zu,\"%s\"", channel, tx_len * 2, tx_hex);
    ESP_LOGD(TAG, "CGLA TX: ch=%s chars=%zu data=%s", channel, tx_len * 2, tx_hex);

    char *resp_buf = (char *)malloc(APDU_MAX_RESP);
    if (!resp_buf) {
        set_error("內存不足");
        return ESP_ERR_NO_MEM;
    }
    memset(resp_buf, 0, APDU_MAX_RESP);

    esp_err_t err = modem_send_at_command(cmd, resp_buf, APDU_MAX_RESP, 30000);
    if (err != ESP_OK) {
        set_error("CGLA 發送失敗");
        free(resp_buf);
        return err;
    }

    char *p = strstr(resp_buf, "+CGLA:");
    if (p) p += 6;
    else p = resp_buf;

    char *comma = strchr(p, ',');
    char *hex_start = comma ? (comma + 1) : p;

    char *clean_hex = (char *)malloc(strlen(hex_start) + 1);
    if (!clean_hex) {
        free(resp_buf);
        return ESP_ERR_NO_MEM;
    }

    size_t clean_len = 0;
    for (size_t i = 0; hex_start[i] != '\0'; i++) {
        if (is_hex_char(hex_start[i])) {
            clean_hex[clean_len++] = hex_start[i];
        }
    }
    clean_hex[clean_len] = '\0';

    if (clean_len == 0 || (clean_len % 2) != 0) {
        set_error("十六進制清洗異常");
        free(clean_hex);
        free(resp_buf);
        return ESP_FAIL;
    }

    size_t byte_len = clean_len / 2;
    *rx = (uint8_t *)malloc(byte_len);
    if (!*rx) {
        free(clean_hex);
        free(resp_buf);
        return ESP_ERR_NO_MEM;
    }

    *rx_len = hex_to_bytes(clean_hex, *rx, byte_len);

    free(clean_hex);
    free(resp_buf);
    return ESP_OK;
}

// ==================== ES10x 封裝 ====================

static esp_err_t es10x_command(const uint8_t *der_req, size_t der_req_len, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;

    char channel[8] = {0};
    esp_err_t err = open_channel(channel, sizeof(channel));
    if (err != ESP_OK) return err;

    uint8_t cla = class_byte_for_channel(channel);
    uint8_t apdu[260];

    if (der_req_len > 255) {
        close_channel(channel);
        return ESP_ERR_INVALID_ARG;
    }

    apdu[0] = cla;
    apdu[1] = 0xE2; 
    apdu[2] = 0x91; 
    apdu[3] = 0x00; 
    apdu[4] = (uint8_t)der_req_len;
    memcpy(apdu + 5, der_req, der_req_len);

    uint8_t *resp = NULL;
    size_t resp_len = 0;
    err = transmit_apdu(channel, apdu, 5 + der_req_len, &resp, &resp_len);

    if (err != ESP_OK) {
        close_channel(channel);
        return err;
    }

    if (resp_len < 2) {
        close_channel(channel);
        free(resp);
        return ESP_FAIL;
    }

    uint8_t sw1 = resp[resp_len - 2];
    uint8_t sw2 = resp[resp_len - 1];
    size_t data_len = resp_len - 2;

    if (sw1 == 0x61) {
        size_t total_len = data_len;
        uint8_t *total = malloc(total_len > 0 ? total_len : 1);
        if (total_len > 0) memcpy(total, resp, data_len);
        free(resp);

        while (sw1 == 0x61) {
            uint8_t le = sw2;           
            if (le == 0) le = 0x00;
            uint8_t get_resp[5] = {cla, 0xC0, 0x00, 0x00, le};
            uint8_t *chunk = NULL;
            size_t chunk_len = 0;

            err = transmit_apdu(channel, get_resp, 5, &chunk, &chunk_len);
            if (err != ESP_OK || chunk_len < 2) {
                free(chunk);
                free(total);
                close_channel(channel);
                return ESP_FAIL;
            }

            sw1 = chunk[chunk_len - 2];
            sw2 = chunk[chunk_len - 1];
            size_t chunk_data_len = chunk_len - 2;

            uint8_t *new_total = (uint8_t *)realloc(total, total_len + chunk_data_len + 1);
            if (!new_total) {
                free(chunk);
                free(total);
                close_channel(channel);
                return ESP_ERR_NO_MEM;
            }
            if (chunk_data_len > 0) memcpy(new_total + total_len, chunk, chunk_data_len);
            total_len += chunk_data_len;
            total = new_total;
            free(chunk);
        }

        *out = total;
        *out_len = total_len;
        close_channel(channel);
        return ESP_OK;
    }

    if ((sw1 & 0xF0) == 0x90) {
        *out = (uint8_t *)malloc(data_len > 0 ? data_len : 1);
        if (data_len > 0) memcpy(*out, resp, data_len);
        *out_len = data_len;
        free(resp);
        close_channel(channel);
        return ESP_OK;
    }

    set_error("APDU 失敗 SW: %02X%02X", sw1, sw2);
    free(resp);
    close_channel(channel);
    return ESP_FAIL;
}

// ==================== Profile 操作 ====================

static esp_err_t build_profile_identifier(const char *id_text, uint8_t *out, size_t out_size, size_t *out_len) {
    *out_len = 0;
    size_t id_len = strlen(id_text);
    if (id_len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t id_bytes[16];
    size_t id_bytes_len = 0;

    if (id_len == 32 && is_hex_string(id_text)) {
        id_bytes_len = hex_to_bytes(id_text, id_bytes, sizeof(id_bytes));
        append_tlv(out, out_len, 0x4F, id_bytes, id_bytes_len);
    } else {
        uint8_t bcd[10];
        size_t bcd_len = 0;
        if (!iccid_to_bcd(id_text, bcd, sizeof(bcd), &bcd_len)) return ESP_ERR_INVALID_ARG;
        append_tlv(out, out_len, 0x5A, bcd, bcd_len);
    }
    return (*out_len <= out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t send_profile_apdu_flavor(uint32_t outer_tag, const uint8_t *id_tlv, size_t id_len, bool refresh, int flavor, int *out_sw) {
    uint8_t req[64];
    size_t pos = 0;
    uint8_t ref_val = refresh ? 0xFF : 0x00;
    uint8_t ref_tlv[4];
    size_t ref_len = 0;
    append_tlv(ref_tlv, &ref_len, 0x81, &ref_val, 1);

    if (flavor == 1) {
        // Flavor 1 (對齊 esim.cpp 的標準格式): BF31 -> [ A0 -> 5A/4F_TLV ] + [ 81_TLV ]
        uint8_t a0_tlv[32];
        size_t a0_len = 0;
        append_tlv(a0_tlv, &a0_len, 0xA0, id_tlv, id_len);

        uint8_t payload[40];
        size_t p_len = 0;
        memcpy(payload, a0_tlv, a0_len);
        p_len += a0_len;
        memcpy(payload + p_len, ref_tlv, ref_len);
        p_len += ref_len;
        append_tlv(req, &pos, outer_tag, payload, p_len);
    } else if (flavor == 2) {
        // Flavor 2 (對齊 esim.cpp 的 LPAC 兼容嵌套流): BF31 -> [ A0 -> ( 5A/4F_TLV + 81_TLV ) ]
        uint8_t inner[40];
        size_t in_len = 0;
        memcpy(inner, id_tlv, id_len);
        in_len += id_len;
        memcpy(inner + in_len, ref_tlv, ref_len);
        in_len += ref_len;

        uint8_t a0_tlv[45];
        size_t a0_len = 0;
        append_tlv(a0_tlv, &a0_len, 0xA0, inner, in_len);
        append_tlv(req, &pos, outer_tag, a0_tlv, a0_len);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *resp = NULL;
    size_t resp_len = 0;
    esp_err_t err = es10x_command(req, pos, &resp, &resp_len);
    if (err != ESP_OK) return err;

    tlv_node_t top, res_node;
    if (read_tlv(resp, resp_len, 0, &top) && top.tag == outer_tag) {
        
        // 抓取 Success (0x80) 或 (0xA0 構造) 節點
        if (find_child_tag(top.value, top.length, 0x80, &res_node) ||
            find_child_tag(top.value, top.length, 0xA0, &res_node)) {
            
            ESP_LOGI(TAG, "卡片返回 Success 容器 (0x80/0xA0)，底層已執行成功！");
            if (out_sw) *out_sw = 0;
            free(resp);
            return ESP_OK;
        }

        // 抓取 Error (0x81) 節點
        if (find_child_tag(top.value, top.length, 0x81, &res_node)) {
            int actual_err = 0;
            for (size_t i = 0; i < res_node.length; i++) {
                actual_err = (actual_err << 8) | res_node.value[i];
            }
            ESP_LOGW(TAG, "卡片亮出 Error 標籤(0x81)，拒絕碼: %d", actual_err);
            if (out_sw) *out_sw = actual_err;
            free(resp);
            return ESP_FAIL;
        }
    }

    if (out_sw) *out_sw = -1;
    free(resp);
    return ESP_FAIL;
}

static esp_err_t profile_operation(uint32_t outer_tag, const char *id_text, bool refresh, bool enable_for_reason, bool lpac_nested_format, int *out_sw) {
    if (out_sw) *out_sw = -1;
    
    uint8_t id_tlv[24];
    size_t id_len = 0;
    if (build_profile_identifier(id_text, id_tlv, sizeof(id_tlv), &id_len) != ESP_OK) {
        set_error("ID編碼非法");
        return ESP_ERR_INVALID_ARG;
    }

    if (outer_tag == 0xBF33) {
        uint8_t req[32];
        size_t pos = 0;
        append_tlv(req, &pos, outer_tag, id_tlv, id_len);
        uint8_t *resp = NULL;
        size_t resp_len = 0;
        if (es10x_command(req, pos, &resp, &resp_len) == ESP_OK) {
            tlv_node_t top, res_node;
            if (read_tlv(resp, resp_len, 0, &top) && find_child_tag(top.value, top.length, 0x80, &res_node)) {
                int r = 0;
                for (size_t i = 0; i < res_node.length; i++) r = (r << 8) | res_node.value[i];
                if (out_sw) *out_sw = r;
                free(resp);
                return (r == 0) ? ESP_OK : ESP_FAIL;
            }
            free(resp);
        }
        return ESP_FAIL;
    }

    int sw_res = -1;
    esp_err_t err;

    // lpac_nested_format 為 false 時 -> 對應 esim.cpp 的 Standard 格式 (Flavor 1)
    // lpac_nested_format 為 true 時  -> 對應 esim.cpp 的 LPAC 格式 (Flavor 2)
    int flavor = lpac_nested_format ? 2 : 1;

    ESP_LOGD(TAG, "穿甲發包 -> 正在發送方言 Flavor %d", flavor);
    err = send_profile_apdu_flavor(outer_tag, id_tlv, id_len, refresh, flavor, &sw_res);

    if (out_sw) *out_sw = sw_res;

    if (err == ESP_OK && sw_res == 0) {
        return ESP_OK;
    }

    if (sw_res == 1) {
        set_error("卡片明確拒絕: 未找到此 ICCID/AID");
    } else {
        set_error("操作失敗 (拒絕碼=%d)", sw_res);
    }
    return ESP_FAIL;
}

// ==================== 公開 API ====================

esp_err_t esim_manager_init(void) {
    s_esim_supported = false;
    const char *cmds[] = {"AT+CCHO=?", "AT+CCHC=?", "AT+CGLA=?"};
    for (int i = 0; i < 3; i++) {
        char resp[128] = {0};
        if (modem_send_at_command(cmds[i], resp, sizeof(resp), 3000) != ESP_OK || !strstr(resp, "OK")) {
            set_error("基帶不支持 eUICC 指令: %s", cmds[i]);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    s_esim_supported = true;
    return ESP_OK;
}

esp_err_t esim_manager_get_eid(char *eid, size_t buf_size) {
    if (!eid || buf_size < 33) return ESP_ERR_INVALID_ARG;
    eid[0] = '\0';

    uint8_t request[] = {0xBF, 0x3E, 0x03, 0x5C, 0x01, 0x5A};
    uint8_t *resp = NULL;
    size_t resp_len = 0;

    esp_err_t err = es10x_command(request, sizeof(request), &resp, &resp_len);
    if (err != ESP_OK) return err;

    bool ok = false;
    tlv_node_t top, eid_node;

    if (read_tlv(resp, resp_len, 0, &top)) {
        if (top.tag == 0xBF3E && find_child_tag(top.value, top.length, 0x5A, &eid_node)) ok = true;
        else if (top.tag == 0x5A) { eid_node = top; ok = true; }
    }
    if (!ok && resp_len == 16) { eid_node.value = resp; eid_node.length = resp_len; ok = true; }

    if (ok && eid_node.length > 0) {
        bytes_to_hex(eid_node.value, eid_node.length, eid, buf_size);
        ESP_LOGI(TAG, "讀取到 EID: %s", eid);
        err = ESP_OK;
    } else {
        set_error("EID 數據段解析失敗");
        err = ESP_FAIL;
    }

    free(resp);
    return err;
}

static void parse_profile_node_content(const uint8_t *data, size_t len, esim_profile_t *p) {
    size_t pos = 0; tlv_node_t c;
    memset(p, 0, sizeof(esim_profile_t));
    p->state = ESIM_PROFILE_UNKNOWN; p->profileClass = -1;

    while (read_tlv(data, len, pos, &c)) {
        pos = c.next_offset;
        switch (c.tag) {
            case 0x5A: bcd_to_iccid(c.value, c.length, p->iccid, sizeof(p->iccid)); break;
            case 0x4F: bytes_to_hex(c.value, c.length, p->isdpAid, sizeof(p->isdpAid)); break;
            case 0x9F70: p->state = (c.length >= 1) ? (esim_profile_state_t)c.value[0] : ESIM_PROFILE_UNKNOWN; break;
            case 0x90: snprintf(p->nickname, sizeof(p->nickname), "%.*s", (int)c.length, c.value); break;
            case 0x91: snprintf(p->serviceProviderName, sizeof(p->serviceProviderName), "%.*s", (int)c.length, c.value); break;
            case 0x92: snprintf(p->profileName, sizeof(p->profileName), "%.*s", (int)c.length, c.value); break;
            case 0x95: if (c.length >= 1) p->profileClass = c.value[0]; break;
        }
    }
}

static void scan_profiles_in_tlv(const uint8_t *data, size_t len, esim_profile_t *profiles, int max_count, int *count) {
    size_t pos = 0; tlv_node_t n;
    while (*count < max_count && read_tlv(data, len, pos, &n)) {
        pos = n.next_offset;
        if (n.tag == 0xE3 || n.tag == 0xBF25) {
            parse_profile_node_content(n.value, n.length, &profiles[*count]);
            (*count)++;
        } else if (n.tag == 0xA0 || n.tag == 0xBF2D || n.tag == 0xBF23) {
            scan_profiles_in_tlv(n.value, n.length, profiles, max_count, count);
        }
    }
}

esp_err_t esim_manager_get_profiles(esim_profile_t *profiles, int max_count, int *out_count) {
    if (!profiles || max_count <= 0 || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0; 
    memset(profiles, 0, sizeof(esim_profile_t) * max_count);

    uint8_t req[] = { 0xBF, 0x2D, 0x0A, 0x5C, 0x08, 0x5A, 0x4F, 0x9F, 0x70, 0x90, 0x91, 0x92, 0x95 };
    uint8_t *resp = NULL; 
    size_t resp_len = 0;
    if (es10x_command(req, sizeof(req), &resp, &resp_len) != ESP_OK) return ESP_FAIL;

    tlv_node_t top; 
    if (!read_tlv(resp, resp_len, 0, &top) || top.tag != 0xBF2D) { 
        free(resp); set_error("列表頭校驗錯"); return ESP_FAIL; 
    }
    
    int count = 0; 
    scan_profiles_in_tlv(top.value, top.length, profiles, max_count, &count); 
    free(resp);

    // 同步到全局 RAM，供智能代理映射使用
    s_cached_profile_count = (count < MAX_CACHED_PROFILES) ? count : MAX_CACHED_PROFILES;
    memcpy(s_cached_profiles, profiles, sizeof(esim_profile_t) * s_cached_profile_count);

    *out_count = count; 
    return ESP_OK;
}

esp_err_t esim_manager_enable_profile(const char *iccid_or_aid, bool refresh) {
    return profile_operation(0xBF31, iccid_or_aid, refresh, true, false, NULL);
}

esp_err_t esim_manager_disable_profile(const char *iccid_or_aid, bool refresh) {
    return profile_operation(0xBF32, iccid_or_aid, refresh, false, false, NULL);
}

esp_err_t esim_manager_delete_profile(const char *iccid_or_aid) {
    return profile_operation(0xBF33, iccid_or_aid, false, false, false, NULL);
}

esp_err_t esim_manager_switch_profile(const char *iccid_or_aid) {
    ESP_LOGI(TAG, "業務層發起切換 Profile，入參: [%s]", iccid_or_aid);

    // 如果卡片有 EF_ICCID 閹割問題，我們利用之前的緩存自動將 ICCID 轉為 AID
    const char *target_aid = iccid_or_aid;
    size_t in_len = strlen(iccid_or_aid);
    if (s_cached_profile_count > 0 && (in_len == 19 || in_len == 20)) {
        for (int i = 0; i < s_cached_profile_count; i++) {
            if (strncmp(s_cached_profiles[i].iccid, iccid_or_aid, in_len) == 0) {
                target_aid = s_cached_profiles[i].isdpAid;
                ESP_LOGI(TAG, "記憶體代理已將 ICCID 轉換為底層 AID: [%s]", target_aid);
                break;
            }
        }
    }

    int sw_res = 0;
    esp_err_t err;

    // 標準模式 (Refresh = true, LPAC = false)
    err = profile_operation(0xBF31, target_aid, true, true, false, &sw_res);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "eSIM 切換成功！");
        return ESP_OK;
    }

    // 降級模式 1 -> 檢測到 CAT busy，改用無刷新重試 (Refresh = false, LPAC = false)
    if (sw_res == 5) {
        ESP_LOGW(TAG, "eSIM 切換返回 CAT busy，改用 refresh=false 重試");
        err = profile_operation(0xBF31, target_aid, false, true, false, &sw_res);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "eSIM 無刷新切換成功，可能需要重啟模組/重新註冊網路後生效");
            return ESP_OK;
        }
    }

    // 降級模式 2 -> LPAC 兼容嵌套格式重試 (Refresh = false, LPAC = true)
    if (sw_res == 5) {
        ESP_LOGW(TAG, "eSIM 仍返回 CAT busy，改用 lpac 兼容格式 refresh=false 重試");
        err = profile_operation(0xBF31, target_aid, false, true, true, &sw_res);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "eSIM lpac兼容格式切換成功，可能需要重啟模組/重新註冊網路後生效");
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "eSIM 切換徹底失敗，最終狀態碼: %d", sw_res);
    return err;
}