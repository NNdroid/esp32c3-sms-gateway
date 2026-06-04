#pragma once

#include "esp_err.h"
#include "esp_http_client.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 全局通用 HTTP/HTTPS 请求函数
 * * 💡 特性：
 * 1. 自动处理网络时间同步等待 (防止 HTTPS 证书因时间错误被拒)
 * 2. 自动进行错误重试与指数退避
 * 3. 严格的内存生命周期管理 (用完即毁，防止 SSL 碎片化)
 * 4. 屏蔽大网页下载导致的 Event Queue Timeout 警告
 *
 * @param url          目标 URL (http:// 或 https://)
 * @param method       HTTP 请求方法 (如 HTTP_METHOD_GET, HTTP_METHOD_POST)
 * @param payload      POST 提交的字符串数据 (如果是 GET，传 NULL)
 * @param content_type Content-Type 头部 (如果是 GET，传 NULL)
 * @return true        请求成功 (状态码 200~299)
 * @return false       请求失败
 */
bool http_util_request(const char *url, esp_http_client_method_t method, const char *payload, const char *content_type);

#ifdef __cplusplus
}
#endif