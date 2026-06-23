#ifndef ESIM_MANAGER_H
#define ESIM_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 最大 profile 数量 */
#define ESIM_MAX_PROFILES 10
/** ICCID 长度 (BCD 19位+结束符) */
#define ESIM_ICCID_LEN 20
/** EID 长度 (十六进制) */
#define ESIM_EID_LEN 40

/**
 * @brief eSIM profile 状态枚举
 */
typedef enum {
    ESIM_PROFILE_DISABLED = 0,  /**< 已禁用 */
    ESIM_PROFILE_ENABLED  = 1,  /**< 已启用 */
    ESIM_PROFILE_UNKNOWN  = -1  /**< 未知状态 */
} esim_profile_state_t;

/**
 * @brief eSIM profile 结构体
 */
typedef struct {
    char iccid[ESIM_ICCID_LEN];           /**< ICCID (BCD编码字符串) */
    char isdpAid[40];                     /**< ISD-P AID (十六进制) */
    esim_profile_state_t state;           /**< 启用状态 */
    char nickname[64];                    /**< 昵称 */
    char serviceProviderName[64];         /**< 运营商名称 */
    char profileName[64];                 /**< Profile 名称 */
    int profileClass;                     /**< Profile 类别 (-1=未知) */
} esim_profile_t;

/**
 * @brief 初始化 eSIM 管理器
 *        检测模组是否支持 eUICC 命令 (AT+CCHO=?, AT+CCHC=?, AT+CGLA=?)
 * 
 * @return ESP_OK 成功 (支持 eSIM)
 * @return ESP_ERR_NOT_SUPPORTED 模组不支持 eUICC
 * @return ESP_FAIL 其他错误
 */
esp_err_t esim_manager_init(void);

/**
 * @brief 检查模组是否支持 eSIM 功能
 */
bool esim_manager_is_supported(void);

/**
 * @brief 获取 EID (eUICC Identifier)
 * 
 * @param eid 输出缓冲区 (至少 ESIM_EID_LEN 字节)
 * @param buf_size 缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t esim_manager_get_eid(char *eid, size_t buf_size);

/**
 * @brief 获取所有 profile 列表
 * 
 * @param profiles 输出数组
 * @param max_count 数组最大容量 (建议 ESIM_MAX_PROFILES)
 * @param out_count 输出实际数量
 * @return ESP_OK 成功
 */
esp_err_t esim_manager_get_profiles(esim_profile_t *profiles, int max_count, int *out_count);

/**
 * @brief 启用一个 profile (通过 ICCID 或 AID)
 * 
 * @param iccid_or_aid ICCID (数字字符串) 或 ISD-P AID (32位HEX)
 * @param refresh 是否启用后立即触发网络刷新
 * @return ESP_OK 成功
 */
esp_err_t esim_manager_enable_profile(const char *iccid_or_aid, bool refresh);

/**
 * @brief 禁用一个 profile
 * 
 * @param iccid_or_aid ICCID 或 AID
 * @param refresh 是否立即刷新
 * @return ESP_OK 成功
 */
esp_err_t esim_manager_disable_profile(const char *iccid_or_aid, bool refresh);

/**
 * @brief 删除一个 profile
 * 
 * @param iccid_or_aid ICCID 或 AID
 * @return ESP_OK 成功
 */
esp_err_t esim_manager_delete_profile(const char *iccid_or_aid);

/**
 * @brief 切换 profile (先启用目标 profile)
 *        如果返回 ESP_ERR_ESIM_CAT_BUSY，可调用 esim_manager_enable_profile 重试
 * 
 * @param iccid_or_aid 目标 ICCID 或 AID
 * @return ESP_OK 成功
 * @return ESP_ERR_ESIM_CAT_BUSY CAT 忙，可稍后重试
 */
#define ESP_ERR_ESIM_CAT_BUSY 0x200

esp_err_t esim_manager_switch_profile(const char *iccid_or_aid);

/**
 * @brief 获取最后一次操作的错误信息
 */
const char *esim_manager_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // ESIM_MANAGER_H
