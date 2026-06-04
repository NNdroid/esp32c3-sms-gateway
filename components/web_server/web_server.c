#include "web_server.h"
#include "sdkconfig.h"
#include "config_manager.h"
#include "modem_driver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_service.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <ctype.h>
#include "esp_ota_ops.h"
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <math.h>

static const char *TAG = "WEB_SRV";
static httpd_handle_t server = NULL;
static SemaphoreHandle_t api_config_mutex = NULL;
static char session_token[33] = {0};

#define WEB_SERVER_MAX_JSON_BODY 1024
#define WEB_SERVER_API_CONFIG_JSON_SIZE 12288
#define WEB_SERVER_MAX_QUERY_BUFFER 256

static float get_cpu_usage_percent(void) {
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    TaskStatus_t pxTaskStatusArray[32];
    volatile UBaseType_t uxArraySize, x;
    uint32_t ulTotalRunTime, ulStatsAsPercentage;
    float total_cpu_usage = 0.0f;

    uxArraySize = uxTaskGetNumberOfTasks();
    if (uxArraySize > sizeof(pxTaskStatusArray) / sizeof(pxTaskStatusArray[0])) {
        uxArraySize = sizeof(pxTaskStatusArray) / sizeof(pxTaskStatusArray[0]);
    }

    uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);
    if (uxArraySize == 0) {
        return total_cpu_usage;
    }

    ulTotalRunTime /= 100UL;
    if (ulTotalRunTime > 0) {
        for (x = 0; x < uxArraySize; x++) {
            if (strncmp(pxTaskStatusArray[x].pcTaskName, "IDLE", 4) == 0) {
                ulStatsAsPercentage = pxTaskStatusArray[x].ulRunTimeCounter / ulTotalRunTime;
                if (ulStatsAsPercentage > 100) ulStatsAsPercentage = 100;
                total_cpu_usage = 100.0f - (float)ulStatsAsPercentage;
                break;
            }
        }
    }
    return total_cpu_usage;
#else
    return 0.0f;
#endif
}

// ================= 嵌入式 HTML 资源引用 =================
extern const uint8_t login_html_start[] asm("_binary_login_html_start");
extern const uint8_t login_html_end[]   asm("_binary_login_html_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t tools_html_start[] asm("_binary_tools_html_start");
extern const uint8_t tools_html_end[]   asm("_binary_tools_html_end");
extern const uint8_t ota_html_start[]   asm("_binary_ota_html_start");
extern const uint8_t ota_html_end[]     asm("_binary_ota_html_end");

// ================= 工具函数 =================
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool read_url_query(httpd_req_t *req, char *query, size_t max_len) {
    if (!req || !query || max_len == 0) {
        return false;
    }
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1) {
        return false;
    }
    if (query_len > max_len) {
        query_len = max_len;
    }
    return httpd_req_get_url_query_str(req, query, query_len) == ESP_OK;
}

static bool read_request_body_safe(httpd_req_t *req, char *buf, size_t buf_len) {
    if (!req || !buf || buf_len == 0 || req->content_len == 0 || req->content_len >= buf_len) {
        return false;
    }
    if (httpd_req_recv(req, buf, req->content_len) <= 0) {
        return false;
    }
    buf[req->content_len] = '\0';
    return true;
}

static const char* skip_json_whitespace(const char *ptr) {
    while (ptr && *ptr && isspace((unsigned char)*ptr)) ptr++;
    return ptr;
}

static bool json_key_compare(const char *json_start, const char *json_end, const char *key) {
    size_t len = json_end - json_start;
    return (strlen(key) == len && strncmp(json_start, key, len) == 0);
}

static const char* json_find_value(const char *json, const char *key) {
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *key_start = p + 1;
        const char *key_end = key_start;
        while (*key_end && *key_end != '"') {
            if (*key_end == '\\' && key_end[1]) key_end += 2;
            else key_end++;
        }
        if (*key_end != '"') break;
        if (json_key_compare(key_start, key_end, key)) {
            const char *value = skip_json_whitespace(key_end + 1);
            if (*value != ':') {
                p = key_end + 1;
                continue;
            }
            value = skip_json_whitespace(value + 1);
            return value;
        }
        p = key_end + 1;
    }
    return NULL;
}

static bool json_parse_string(const char *src, char *dst, size_t dst_len) {
    if (!src || *src != '"' || dst_len == 0) return false;
    src++;
    size_t idx = 0;
    while (*src && *src != '"' && idx + 1 < dst_len) {
        if (*src == '\\') {
            src++;
            if (!*src) break;
            switch (*src) {
                case '"': dst[idx++] = '"'; break;
                case '\\': dst[idx++] = '\\'; break;
                case '/': dst[idx++] = '/'; break;
                case 'b': dst[idx++] = '\b'; break;
                case 'f': dst[idx++] = '\f'; break;
                case 'n': dst[idx++] = '\n'; break;
                case 'r': dst[idx++] = '\r'; break;
                case 't': dst[idx++] = '\t'; break;
                default: dst[idx++] = *src; break;
            }
            src++;
        } else {
            dst[idx++] = *src++;
        }
    }
    dst[idx] = '\0';
    return (*src == '"');
}

static bool json_get_string_value(const char *json, const char *key, char *dst, size_t dst_len) {
    if (!json || !key || !dst || dst_len == 0) return false;
    const char *value = json_find_value(json, key);
    return value && *value == '"' && json_parse_string(value, dst, dst_len);
}

static bool json_get_bool_value(const char *json, const char *key, bool *out_value) {
    if (!json || !key || !out_value) return false;
    const char *value = json_find_value(json, key);
    if (!value) return false;
    if (*value == '"') {
        char temp[16] = {0};
        if (!json_parse_string(value, temp, sizeof(temp))) return false;
        if (strcasecmp(temp, "true") == 0 || strcmp(temp, "1") == 0 || strcasecmp(temp, "on") == 0) {
            *out_value = true; return true;
        }
        if (strcasecmp(temp, "false") == 0 || strcmp(temp, "0") == 0 || strcasecmp(temp, "off") == 0) {
            *out_value = false; return true;
        }
        return false;
    }
    if (strncasecmp(value, "true", 4) == 0) { *out_value = true; return true; }
    if (strncasecmp(value, "false", 5) == 0) { *out_value = false; return true; }
    if (*value == '1') { *out_value = true; return true; }
    if (*value == '0') { *out_value = false; return true; }
    return false;
}

static bool json_get_int_value(const char *json, const char *key, int *out_value) {
    if (!json || !key || !out_value) return false;
    const char *value = json_find_value(json, key);
    if (!value) return false;
    if (*value == '"') {
        char temp[32] = {0};
        if (!json_parse_string(value, temp, sizeof(temp))) return false;
        *out_value = atoi(temp);
        return true;
    }
    char *endptr = NULL;
    long result = strtol(value, &endptr, 10);
    if (value == endptr) return false;
    *out_value = (int)result;
    return true;
}

static bool request_is_json(httpd_req_t *req) {
    if (!req) return false;
    char content_type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        return false;
    }
    return strstr(content_type, "application/json") != NULL;
}

static void escape_json_string(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    size_t remaining = dst_len - 1;
    while (*src && remaining > 0) {
        if (*src == '"' && remaining >= 2) { *dst++ = '\\'; *dst++ = '"'; remaining -= 2; }
        else if (*src == '\\' && remaining >= 2) { *dst++ = '\\'; *dst++ = '\\'; remaining -= 2; }
        else if (*src == '\n' && remaining >= 2) { *dst++ = '\\'; *dst++ = 'n'; remaining -= 2; }
        else if (*src == '\r' && remaining >= 2) { *dst++ = '\\'; *dst++ = 'r'; remaining -= 2; }
        else {
            *dst++ = *src;
            remaining -= 1;
        }
        src++;
    }
    *dst = '\0';
}

// ================= 鉴权辅助 =================
static bool check_cookie_auth(httpd_req_t *req) {
    char buf[128];
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len > 0 && len < sizeof(buf)) {
        if (httpd_req_get_hdr_value_str(req, "Cookie", buf, sizeof(buf)) == ESP_OK) {
            char expected[64];
            snprintf(expected, sizeof(expected), "sessionid=%s", session_token);
            if (strstr(buf, expected) != NULL) return true;
        }
    }
    return false;
}

static inline esp_err_t httpd_resp_send_401(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":false,\"message\":\"Unauthorized\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t page_handler(httpd_req_t *req, const uint8_t* start, const uint8_t* end, bool require_auth) {
    if (require_auth && !check_cookie_auth(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)start, end - start);
}

// ================= 页面路由 =================
static esp_err_t handleRoot(httpd_req_t *req) { return page_handler(req, index_html_start, index_html_end, true); }
static esp_err_t handleToolsPage(httpd_req_t *req) { return page_handler(req, tools_html_start, tools_html_end, true); }
static esp_err_t handleOtaPage(httpd_req_t *req) { return page_handler(req, ota_html_start, ota_html_end, true); }

static esp_err_t handleLogin(httpd_req_t *req) {
    if (check_cookie_auth(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, NULL, 0);
    }
    return page_handler(req, login_html_start, login_html_end, false);
}

static esp_err_t handleDoLogin(httpd_req_t *req) {
    char buf[128];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) return ESP_FAIL;
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char user[32] = {0}, pass[64] = {0};
    if (httpd_query_key_value(buf, "username", user, sizeof(user)) == ESP_OK &&
        httpd_query_key_value(buf, "password", pass, sizeof(pass)) == ESP_OK) {
        if (strcmp(user, g_app_config.webUser) == 0 && strcmp(pass, g_app_config.webPass) == 0) {
            char cookie[128];
            snprintf(cookie, sizeof(cookie), "sessionid=%s; Path=/; Max-Age=2592000; HttpOnly", session_token);
            httpd_resp_set_hdr(req, "Set-Cookie", cookie);
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            return httpd_resp_send(req, NULL, 0);
        }
    }
    return page_handler(req, login_html_start, login_html_end, false);
}

static esp_err_t handleLogout(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Set-Cookie", "sessionid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    return httpd_resp_send(req, NULL, 0);
}

// ================= API: 基础配置与系统 =================
static esp_err_t handleApiConfig(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    if (xSemaphoreTake(api_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }

    char *json = malloc(WEB_SERVER_API_CONFIG_JSON_SIZE);
    if (!json) {
        xSemaphoreGive(api_config_mutex);
        return ESP_ERR_NO_MEM;
    }
    char esc_webUser[128] = {0};
    char esc_webPass[128] = {0};
    char esc_adminPhone[64] = {0};
    char esc_numberBlackList[1024] = {0};
    char esc_syslogServer[128] = {0};
    char esc_plmn[32] = {0};
    char esc_smsc[64] = {0};
    char esc_imei[64] = {0};

    char esc_name[128] = {0};
    char esc_url[512] = {0};
    char esc_key1[128] = {0};
    char esc_key2[128] = {0};
    char esc_body[2048] = {0};
    char esc_phone[64] = {0};
    char esc_content[512] = {0};
    char esc_pingTarget[64] = {0};

    escape_json_string("admin", esc_webUser, sizeof(esc_webUser));
    escape_json_string(g_app_config.webPass, esc_webPass, sizeof(esc_webPass));
    escape_json_string(g_app_config.adminPhone, esc_adminPhone, sizeof(esc_adminPhone));
    escape_json_string(g_app_config.numberBlackList, esc_numberBlackList, sizeof(esc_numberBlackList));
    escape_json_string(g_app_config.syslogServer, esc_syslogServer, sizeof(esc_syslogServer));
    escape_json_string(g_app_config.plmn, esc_plmn, sizeof(esc_plmn));
    escape_json_string(g_app_config.smsc, esc_smsc, sizeof(esc_smsc));
    escape_json_string(g_app_config.imei, esc_imei, sizeof(esc_imei));

    int pos = snprintf(json, WEB_SERVER_API_CONFIG_JSON_SIZE,
        "{\"webUser\":\"%s\",\"webPass\":\"%s\",\"adminPhone\":\"%s\",\"numberBlackList\":\"%s\"," 
        "\"syslogEnabled\":%s,\"syslogServer\":\"%s\",\"syslogPort\":%d,"
        "\"plmn\":\"%s\",\"smsc\":\"%s\",\"imei\":\"%s\",\"cronTaskerEnabled\":%s,\"logServiceEnabled\":%s,\"callProcessorEnabled\":%s,\"callNotifyEnabled\":%s,\"pushChannels\":[",
        esc_webUser, esc_webPass, esc_adminPhone, esc_numberBlackList,
        g_app_config.syslogEnabled ? "true" : "false", esc_syslogServer, g_app_config.syslogPort,
        esc_plmn, esc_smsc, esc_imei,
#if defined(CONFIG_ENABLE_CRON_TASKER) && CONFIG_ENABLE_CRON_TASKER
        "true",
#else
        "false",
#endif
#if defined(CONFIG_ENABLE_LOG_SERVICE) && CONFIG_ENABLE_LOG_SERVICE
        "true",
#else
        "false",
#endif
#if defined(CONFIG_ENABLE_CALL_PROCESSOR) && CONFIG_ENABLE_CALL_PROCESSOR
        "true",
        g_app_config.callNotifyEnabled ? "true" : "false"
#else
        "false",
        "false"
#endif
        );
    if (pos < 0) pos = 0;
    if (pos >= WEB_SERVER_API_CONFIG_JSON_SIZE) pos = WEB_SERVER_API_CONFIG_JSON_SIZE - 1;

    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        escape_json_string(g_app_config.pushChannels[i].name, esc_name, sizeof(esc_name));
        escape_json_string(g_app_config.pushChannels[i].url, esc_url, sizeof(esc_url));
        escape_json_string(g_app_config.pushChannels[i].key1, esc_key1, sizeof(esc_key1));
        escape_json_string(g_app_config.pushChannels[i].key2, esc_key2, sizeof(esc_key2));
        escape_json_string(g_app_config.pushChannels[i].customBody, esc_body, sizeof(esc_body));

        int written = snprintf(json + pos, WEB_SERVER_API_CONFIG_JSON_SIZE - pos,
            "{\"enabled\":%s,\"type\":%d,\"name\":\"%s\",\"url\":\"%s\",\"key1\":\"%s\",\"key2\":\"%s\",\"body\":\"%s\"}%s",
            g_app_config.pushChannels[i].enabled ? "true" : "false", g_app_config.pushChannels[i].type,
            esc_name, esc_url, esc_key1, esc_key2, esc_body,
            (i == MAX_PUSH_CHANNELS - 1) ? "]," : ",");
        if (written < 0) written = 0;
        if (written >= WEB_SERVER_API_CONFIG_JSON_SIZE - pos) written = WEB_SERVER_API_CONFIG_JSON_SIZE - pos - 1;
        pos += written;
    }

    int written = snprintf(json + pos, WEB_SERVER_API_CONFIG_JSON_SIZE - pos, "\"cronTasks\":[");
    if (written < 0) written = 0;
    if (written >= WEB_SERVER_API_CONFIG_JSON_SIZE - pos) written = WEB_SERVER_API_CONFIG_JSON_SIZE - pos - 1;
    pos += written;

    for (int i = 0; i < MAX_CRON_TASKS; i++) {
        escape_json_string(g_app_config.cronTasks[i].phone, esc_phone, sizeof(esc_phone));
        escape_json_string(g_app_config.cronTasks[i].content, esc_content, sizeof(esc_content));
        escape_json_string(g_app_config.cronTasks[i].pingTarget, esc_pingTarget, sizeof(esc_pingTarget));

        written = snprintf(json + pos, WEB_SERVER_API_CONFIG_JSON_SIZE - pos,
            "{\"enabled\":%s,\"type\":%d,\"hour\":%d,\"minute\":%d,\"daysInterval\":%d,\"phone\":\"%s\",\"content\":\"%s\",\"pingTarget\":\"%s\"}%s",
            g_app_config.cronTasks[i].enabled ? "true" : "false", g_app_config.cronTasks[i].type,
            g_app_config.cronTasks[i].hour, g_app_config.cronTasks[i].minute, g_app_config.cronTasks[i].daysInterval,
            esc_phone, esc_content, esc_pingTarget,
            (i == MAX_CRON_TASKS - 1) ? "]}" : ",");
        if (written < 0) written = 0;
        if (written >= WEB_SERVER_API_CONFIG_JSON_SIZE - pos) written = WEB_SERVER_API_CONFIG_JSON_SIZE - pos - 1;
        pos += written;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    xSemaphoreGive(api_config_mutex);
    return res;
}

static esp_err_t handleSave(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    if (!request_is_json(req)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"Content-Type must be json\"}", HTTPD_RESP_USE_STRLEN);
    }
    char *body = malloc(4096);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
    }
    memset(body, 0, 4096);
    if (!read_request_body_safe(req, body, 4096)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"Empty body or body too large\"}", HTTPD_RESP_USE_STRLEN);
    }

    strncpy(g_app_config.webUser, "admin", sizeof(g_app_config.webUser) - 1);
    g_app_config.webUser[sizeof(g_app_config.webUser) - 1] = '\0';

    char newPass[128] = {0};
    json_get_string_value(body, "webPass", newPass, sizeof(newPass));
    if (newPass[0]) {
        strncpy(g_app_config.webPass, newPass, sizeof(g_app_config.webPass) - 1);
        g_app_config.webPass[sizeof(g_app_config.webPass) - 1] = '\0';
    }

    json_get_string_value(body, "adminPhone", g_app_config.adminPhone, sizeof(g_app_config.adminPhone));
    json_get_string_value(body, "numberBlackList", g_app_config.numberBlackList, sizeof(g_app_config.numberBlackList));

#if defined(CONFIG_ENABLE_LOG_SERVICE) && CONFIG_ENABLE_LOG_SERVICE
    json_get_string_value(body, "syslogServer", g_app_config.syslogServer, sizeof(g_app_config.syslogServer));
    bool syslogEnabled = false;
    if (json_get_bool_value(body, "syslogEnabled", &syslogEnabled)) g_app_config.syslogEnabled = syslogEnabled;
    int syslogPort = 0;
    if (json_get_int_value(body, "syslogPort", &syslogPort)) g_app_config.syslogPort = syslogPort;
#else
    (void)body;
#endif

#if defined(CONFIG_ENABLE_CALL_PROCESSOR) && CONFIG_ENABLE_CALL_PROCESSOR
    bool callNotifyEnabled = false;
    if (json_get_bool_value(body, "callNotifyEnabled", &callNotifyEnabled)) {
        g_app_config.callNotifyEnabled = callNotifyEnabled;
    }
#endif

    json_get_string_value(body, "plmn", g_app_config.plmn, sizeof(g_app_config.plmn));
    json_get_string_value(body, "smsc", g_app_config.smsc, sizeof(g_app_config.smsc));
    json_get_string_value(body, "imei", g_app_config.imei, sizeof(g_app_config.imei));

    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        char key[32];
        bool enabled = false; int type = 0;
        snprintf(key, sizeof(key), "push%den", i);
        if (json_get_bool_value(body, key, &enabled)) g_app_config.pushChannels[i].enabled = enabled;
        snprintf(key, sizeof(key), "push%dtype", i);
        if (json_get_int_value(body, key, &type)) g_app_config.pushChannels[i].type = type;
        snprintf(key, sizeof(key), "push%dname", i);
        json_get_string_value(body, key, g_app_config.pushChannels[i].name, sizeof(g_app_config.pushChannels[i].name));
        snprintf(key, sizeof(key), "push%durl", i);
        json_get_string_value(body, key, g_app_config.pushChannels[i].url, sizeof(g_app_config.pushChannels[i].url));
        snprintf(key, sizeof(key), "push%dkey1", i);
        json_get_string_value(body, key, g_app_config.pushChannels[i].key1, sizeof(g_app_config.pushChannels[i].key1));
        snprintf(key, sizeof(key), "push%dkey2", i);
        json_get_string_value(body, key, g_app_config.pushChannels[i].key2, sizeof(g_app_config.pushChannels[i].key2));
        snprintf(key, sizeof(key), "push%dbody", i);
        json_get_string_value(body, key, g_app_config.pushChannels[i].customBody, sizeof(g_app_config.pushChannels[i].customBody));
    }

    for (int i = 0; i < MAX_CRON_TASKS; i++) {
        char key[32];
        bool enabled = false; int type = 0, interval = 0;
        snprintf(key, sizeof(key), "cron%den", i);
        if (json_get_bool_value(body, key, &enabled)) g_app_config.cronTasks[i].enabled = enabled;
        snprintf(key, sizeof(key), "cron%dtype", i);
        if (json_get_int_value(body, key, &type)) g_app_config.cronTasks[i].type = type;
        snprintf(key, sizeof(key), "cron%dinterval", i);
        if (json_get_int_value(body, key, &interval)) g_app_config.cronTasks[i].daysInterval = interval;
        
        snprintf(key, sizeof(key), "cron%dtime", i);
        char time_str[16] = {0};
        json_get_string_value(body, key, time_str, sizeof(time_str));
        if (time_str[0]) {
            char *colon = strchr(time_str, ':');
            if (colon) {
                *colon = '\0';
                g_app_config.cronTasks[i].hour = atoi(time_str);
                g_app_config.cronTasks[i].minute = atoi(colon + 1);
            }
        }
        snprintf(key, sizeof(key), "cron%dphone", i);
        json_get_string_value(body, key, g_app_config.cronTasks[i].phone, sizeof(g_app_config.cronTasks[i].phone));
        snprintf(key, sizeof(key), "cron%dcontent", i);
        json_get_string_value(body, key, g_app_config.cronTasks[i].content, sizeof(g_app_config.cronTasks[i].content));
        snprintf(key, sizeof(key), "cron%dtarget", i);
        json_get_string_value(body, key, g_app_config.cronTasks[i].pingTarget, sizeof(g_app_config.cronTasks[i].pingTarget));
    }

    config_save_all();

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, "{\"success\":true,\"message\":\"配置已成功保存！\"}", HTTPD_RESP_USE_STRLEN);
    free(body);
    return res;
}

static esp_err_t handleToggleData(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    
    char state_str[16] = {0};
    char query[WEB_SERVER_MAX_QUERY_BUFFER];
    if (read_url_query(req, query, sizeof(query))) {
        httpd_query_key_value(query, "state", state_str, sizeof(state_str));
    }

    // 1. 先校验，后转换
    if (strcmp(state_str, "1") != 0 && strcmp(state_str, "0") != 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request"); // 明确指出请求参数错误
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"Invalid state\"}", HTTPD_RESP_USE_STRLEN);
    }
    int state = atoi(state_str);

    bool command_ok = toggle_modem_data_network(state == 1) == ESP_OK;

    // 等待网络状态改变
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 3. 验证最终状态
    bool data_network_enabled = is_modem_data_allowed();
    bool success = (data_network_enabled == (state == 1));

    // 4. 将 command_ok 加入响应，有助于后期排错
    char json[128];
    snprintf(json, sizeof(json), "{\"success\":%s,\"dataNetworkEnabled\":%s,\"commandOk\":%s}", 
             success ? "true" : "false", 
             data_network_enabled ? "true" : "false",
             command_ok ? "true" : "false");
             
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleApiSysInfo(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);

    char ip_str[32] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        }
    }

    modem_net_status_t modem_status = modem_get_cellular_status();
    bool cellular_connected = modem_status == MODEM_NET_STATUS_REGISTERED_HOME || modem_status == MODEM_NET_STATUS_REGISTERED_ROAMING;
    bool data_network_enabled = is_modem_data_allowed();
    bool has_gnss = modem_has_gnss();
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    float cpu_usage = get_cpu_usage_percent();

    char resp_json[320];
    snprintf(resp_json, sizeof(resp_json), 
             "{\"ip\":\"%s\",\"cellular\":%s,\"dataNetworkEnabled\":%s, \"version\":\"Build: " __DATE__ " " __TIME__ "\", \"gnssSupport\":%s, \"freeHeap\":%u, \"minFreeHeap\":%u, \"cpuUsage\":%.2f}", 
             ip_str,
             cellular_connected ? "true" : "false",
             data_network_enabled ? "true" : "false",
             has_gnss ? "true" : "false",
             free_heap,
             min_free_heap,
             cpu_usage);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp_json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleReboot(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"设备正在重启...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ================= OTA 接口 =================
static esp_err_t handleApiUpdate(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);

    ESP_LOGI(TAG, "开始 OTA 升级...");
    esp_err_t err;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败 (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[1024];
    int received;
    int remaining = req->content_len;

    while (remaining > 0) {
        if ((received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(update_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        err = esp_ota_write(update_handle, buf, received);
        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(update_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA 升级成功，准备重启");
            httpd_resp_sendstr(req, "OK");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "OTA 结束或设置启动分区失败");
    httpd_resp_send_500(req);
    return ESP_FAIL;
}

// ================= API: 调制解调器相关功能 =================
static esp_err_t handleGetImei(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    char resp_buf[256] = {0}; char imei[32] = "未知"; char json[128];
    esp_err_t err = modem_send_at_command("AT+CGSN=1", resp_buf, sizeof(resp_buf), 2000);
    if (err != ESP_OK || strstr(resp_buf, "ERROR") != NULL || strlen(resp_buf) < 15) {
        memset(resp_buf, 0, sizeof(resp_buf));
        modem_send_at_command("AT+EGMR=0,7", resp_buf, sizeof(resp_buf), 2000);
    }
    int consecutive_digits = 0; int start_index = -1; int len = strlen(resp_buf);
    for (int i = 0; i < len; i++) {
        if (isdigit((unsigned char)resp_buf[i])) {
            if (consecutive_digits == 0) start_index = i;
            consecutive_digits++;
            if (consecutive_digits == 15) {
                if (i == len - 1 || !isdigit((unsigned char)resp_buf[i + 1])) {
                    strncpy(imei, resp_buf + start_index, 15); imei[15] = '\0'; break;
                } else { consecutive_digits = 0; }
            }
        } else { consecutive_digits = 0; }
    }
    snprintf(json, sizeof(json), "{\"success\":true,\"imei\":\"%s\"}", imei);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleSetImei(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":false,\"message\":\"此固件版本暂不支持修改 IMEI\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleSmsc(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    char json[1024] = {0};
    if (req->method == HTTP_GET) {
        char smsc[64] = {0};
        if (modem_get_smsc(smsc, sizeof(smsc)) == ESP_OK) {
            snprintf(json, sizeof(json), "{\"success\":true,\"smsc\":\"%s\"}", smsc);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"smsc\":null}", HTTPD_RESP_USE_STRLEN);
    } else if (req->method == HTTP_POST) {
        if (!request_is_json(req)) {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"success\":false,\"message\":\"Content-Type must be application/json\"}", HTTPD_RESP_USE_STRLEN);
        }
        char buf[WEB_SERVER_MAX_JSON_BODY] = {0};
        if (!read_request_body_safe(req, buf, sizeof(buf))) {
            return ESP_FAIL;
        }
        char smsc[32] = {0};
        json_get_string_value(buf, "smsc", smsc, sizeof(smsc));
        if (strlen(smsc) == 0) {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"success\":false,\"message\":\"SMSC 号码不能为空\"}", HTTPD_RESP_USE_STRLEN);
        }
        bool success = (modem_set_smsc(smsc) == ESP_OK);
        snprintf(json, sizeof(json), "{\"success\":%s,\"message\":\"%s\"}", success ? "true" : "false", success ? "✅ SMSC 修改成功！" : "SMSC 修改失败");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_FAIL;
}

static esp_err_t handleSendSms(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    if (!request_is_json(req)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"Content-Type must be application/json\"}", HTTPD_RESP_USE_STRLEN);
    }
    char buf[WEB_SERVER_MAX_JSON_BODY] = {0};
    if (!read_request_body_safe(req, buf, sizeof(buf))) {
        return ESP_FAIL;
    }
    char phone[32] = {0}, content[256] = {0};
    json_get_string_value(buf, "phone", phone, sizeof(phone));
    json_get_string_value(buf, "content", content, sizeof(content));
    
    esp_err_t err = modem_send_sms_text(phone, content, 10000);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"success\":true,\"message\":\"短信已成功发送\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, "{\"success\":false,\"message\":\"短信发送失败\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handlePing(httpd_req_t *req) {
    if (!check_cookie_auth(req)) {
        return httpd_resp_send_401(req);
    }

    char target[64] = {0};
    
    if (req->content_len > 0) {
        if (!request_is_json(req)) {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"success\":false,\"message\":\"Content-Type must be application/json\"}", HTTPD_RESP_USE_STRLEN);
        }
        char buf[WEB_SERVER_MAX_JSON_BODY] = {0};
        if (!read_request_body_safe(req, buf, sizeof(buf))) {
            return ESP_FAIL;
        }
        json_get_string_value(buf, "target", target, sizeof(target));
    }
    
    if (strlen(target) == 0) {
        strcpy(target, "8.8.8.8");
    }

    char msg_buf[512] = {0};
    char json_buf[1024] = {0};
    char escaped_msg[1024] = {0};

    int successCount = 0;
    int avgRtt = 0;
    char details[512] = {0};
    esp_err_t err = modem_ping(target, 4, 30, &successCount, &avgRtt, details, sizeof(details));
    bool isSuccess = (successCount > 0);
    bool has_error = (err != ESP_OK);

    if (isSuccess) {
        snprintf(msg_buf, sizeof(msg_buf), "發送 4 次，成功 %d 次。\n平均延時: %d ms\n\n%s", successCount, avgRtt, details);
    } else {
        snprintf(msg_buf, sizeof(msg_buf), "4 次 Ping 均超時或失敗，請檢查網路或目標地址。%s", has_error ? "\n底層響應有誤或未开启数据连接" : "");
    }

    escape_json_string(msg_buf, escaped_msg, sizeof(escaped_msg));
    snprintf(json_buf, sizeof(json_buf), "{\"success\":%s,\"message\":\"%s\"}", isSuccess ? "true" : "false", escaped_msg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleResetNetwork(httpd_req_t *req) {
    ESP_LOGW(TAG, "⚠️ 收到重置网络请求，准备清除 Wi-Fi 凭据并重启...");

    // 先给前端回复一个成功的 JSON，以免页面卡死或报错
    httpd_resp_set_type(req, "application/json");
    const char* resp_str = "{\"status\":\"success\", \"message\":\"网络已重置，设备即将重启进入配网模式\"}";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // 延迟 1 秒，确保 HTTP 响应已经成功发送给浏览器
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 清除 Wi-Fi 凭据 (只清 Wi-Fi，不影响你的其他业务配置)
    esp_wifi_restore();

    // 重启 ESP32
    esp_restart();

    return ESP_OK;
}

static esp_err_t handleQuery(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);

    char type[32] = {0};
    char query[WEB_SERVER_MAX_QUERY_BUFFER];
    if (read_url_query(req, query, sizeof(query))) {
        httpd_query_key_value(query, "type", type, sizeof(type));
    }

    char resp_buf[1024] = {0};
    char msg_buf[1024] = {0};
    char json_buf[2048] = {0};

    bool success = false;
    if (strcmp(type, "ati") == 0) {
        if (modem_send_at_command("ATI", resp_buf, sizeof(resp_buf), 2000) == ESP_OK && strstr(resp_buf, "OK")) {
            success = true; char *line1="未知", *line2="未知", *line3="未知";
            char *saveptr; char *token = strtok_r(resp_buf, "\r\n", &saveptr);
            int num = 0;
            while(token) {
                if (strcmp(token,"ATI")!=0 && strcmp(token,"OK")!=0) {
                    num++; if (num==1) line1=token; else if (num==2) line2=token; else if (num==3) line3=token;
                }
                token = strtok_r(NULL, "\r\n", &saveptr);
            }
            snprintf(msg_buf, sizeof(msg_buf), "<table class='info-table'><tr><td>製造商</td><td>%s</td></tr><tr><td>型號</td><td>%s</td></tr><tr><td>版本</td><td>%s</td></tr></table>", line1, line2, line3);
        } else strcpy(msg_buf, "查詢失敗");
    } else if (strcmp(type, "signal") == 0) {
        if (modem_send_at_command("AT+CESQ", resp_buf, sizeof(resp_buf), 2000) == ESP_OK && strstr(resp_buf, "+CESQ:")) {
            success = true; char *p = strstr(resp_buf, "+CESQ:") + 6;
            int v[6] = {0}; sscanf(p, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            char rsrpStr[32], rsrqStr[32];
            if (v[5] == 99 || v[5] == 255) strcpy(rsrpStr, "未知"); else snprintf(rsrpStr, sizeof(rsrpStr), "%d dBm", -140 + v[5]);
            if (v[4] == 99 || v[4] == 255) strcpy(rsrqStr, "未知"); else snprintf(rsrqStr, sizeof(rsrqStr), "%.1f dB", -19.5 + v[4] * 0.5);
            for (int i=0; p[i]; i++) { if (p[i]=='\r'||p[i]=='\n') { p[i]='\0'; break; } }
            snprintf(msg_buf, sizeof(msg_buf), "<table class='info-table'><tr><td>RSRP</td><td>%s</td></tr><tr><td>RSRQ</td><td>%s</td></tr><tr><td>原始數據</td><td>%s</td></tr></table>", rsrpStr, rsrqStr, p);
        } else strcpy(msg_buf, "查詢失敗");
    } else if (strcmp(type, "siminfo") == 0) {
        success = true; char imsi[32] = "未知", iccid[32] = "未知", num[32] = "未儲存";
        if (modem_send_at_command("AT+CIMI", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *start = NULL;
            for(int i=0; resp_buf[i]; i++) { if (isdigit((unsigned char)resp_buf[i]) && isdigit((unsigned char)resp_buf[i+1])) { start = &resp_buf[i]; break; } }
            if (start) { for(int i=0; i<sizeof(imsi)-1 && isdigit((unsigned char)start[i]); i++) { imsi[i] = start[i]; imsi[i+1] = '\0'; } }
        }
        if (modem_send_at_command("AT+ICCID", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, "+ICCID:");
            if (p) { p+=7; while(*p==' ') p++; for(int i=0; i<sizeof(iccid)-1 && (isdigit((unsigned char)p[i]) || isalpha((unsigned char)p[i])); i++) { iccid[i]=p[i]; iccid[i+1]='\0'; } }
        }
        if (modem_send_at_command("AT+CNUM", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, ",\"");
            if (p) { p+=2; char *e = strchr(p, '"'); if (e && (e-p)<sizeof(num)) { strncpy(num, p, e-p); num[e-p]='\0'; } }
        }
        snprintf(msg_buf, sizeof(msg_buf), "<table class='info-table'><tr><td>IMSI</td><td>%s</td></tr><tr><td>ICCID</td><td>%s</td></tr><tr><td>本機號碼</td><td>%s</td></tr></table>", imsi, iccid, num);
    } else if (strcmp(type, "network") == 0) {
        success = true; char reg[32] = "斷開/未註冊", op[32] = "未知", cgact[32] = "未激活", apn[32] = "未知";
        if (modem_send_at_command("AT+CEREG?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) { if (strstr(resp_buf, ",1") || strstr(resp_buf, ",5")) strcpy(reg, "已附著/註冊"); }
        if (modem_send_at_command("AT+COPS?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) { char *p = strstr(resp_buf, ",\""); if (p) { p+=2; char *e=strchr(p,'"'); if (e) { strncpy(op, p, e-p); op[e-p]='\0'; } } }
        if (modem_send_at_command("AT+CGACT?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) { if (strstr(resp_buf, "+CGACT: 1,1")) strcpy(cgact, "已激活"); }
        if (modem_send_at_command("AT+CGDCONT?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) { char *p = strstr(resp_buf, ",\""); if (p) { p=strstr(p+2, ",\""); if(p){ p+=2; char *e=strchr(p,'"'); if(e){ strncpy(apn,p,e-p); apn[e-p]='\0';} } } }
        snprintf(msg_buf, sizeof(msg_buf), "<table class='info-table'><tr><td>註冊狀態</td><td>%s</td></tr><tr><td>電信商</td><td>%s</td></tr><tr><td>數據連接</td><td>%s</td></tr><tr><td>APN</td><td>%s</td></tr></table>", reg, op, cgact, apn);
    } else if (strcmp(type, "wifi") == 0) {
        success = true;
        wifi_ap_record_t ap_info;
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(msg_buf, sizeof(msg_buf), "<table class='info-table'><tr><td>連接狀態</td><td>已連接</td></tr><tr><td>SSID</td><td>%s</td></tr><tr><td>RSSI</td><td>%d dBm</td></tr><tr><td>IP</td><td>" IPSTR "</td></tr></table>", ap_info.ssid, ap_info.rssi, IP2STR(&ip_info.ip));
        } else {
            snprintf(msg_buf, sizeof(msg_buf), "<table class='info-table'><tr><td>連接狀態</td><td>未連接</td></tr></table>");
        }
    } else if (strcmp(type, "cellip") == 0) {
        if (modem_send_at_command("AT+CGPADDR", resp_buf, sizeof(resp_buf), 2000) == ESP_OK && strstr(resp_buf, "+CGPADDR:")) {
            success = true;
            strcpy(msg_buf, "<table class='info-table'><tr><th>CID (上下文)</th><th>IP 位址 (IPv4 / IPv6)</th></tr>");
            char *p = resp_buf;
            while ((p = strstr(p, "+CGPADDR: "))) {
                p += 10;
                char cid[8] = {0}, ip[64] = {0};
                char *comma = strchr(p, ',');
                char *end = strchr(p, '\r'); if (!end) end = strchr(p, '\n');
                
                if (comma && end && comma < end) {
                    strncpy(cid, p, comma - p); cid[comma - p] = '\0';
                    char raw_ip[128] = {0};
                    strncpy(raw_ip, comma + 1, end - comma - 1); raw_ip[end - comma - 1] = '\0';
                    int k = 0;
                    for (int j=0; raw_ip[j]; j++) if (raw_ip[j] != '"') ip[k++] = raw_ip[j];
                    ip[k] = '\0';
                }
                char row[128];
                snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td></tr>", cid, ip);
                strcat(msg_buf, row);
            }
            strcat(msg_buf, "</table>");
        } else {
            strcpy(msg_buf, "查詢失敗，模組可能尚未分配IP");
        }
    } else if (strcmp(type, "gnss") == 0) {
    if (!modem_has_gnss()) {
        strcpy(msg_buf, "❌ 当前模组硬件不支持 GNSS 功能");
    } else {
        // 1. 发送开启指令 (底层的 NULL 不会再打出 null 了)
        modem_set_gnss_state(true); 

        gnss_location_t loc;
        // 2. 瞬间读取底层不断更新的全局缓存
        bool is_fixed = modem_get_location(&loc);

        if (is_fixed) {
            success = true;
            const char* lat_dir = (loc.latitude >= 0) ? "N" : "S";
            const char* lon_dir = (loc.longitude >= 0) ? "E" : "W";

            snprintf(msg_buf, sizeof(msg_buf), 
                "<table class='info-table'>"
                "<tr><td>GNSS状态</td><td>🟢 定位成功 (卫星数: %d)</td></tr>"
                "<tr><td>纬度</td><td>%.6f °%s</td></tr>"
                "<tr><td>经度</td><td>%.6f °%s</td></tr>"
                "<tr><td>时速</td><td>%.1f km/h</td></tr>"
                "<tr><td>高度</td><td>%.1f m</td></tr>"
                "<tr><td>地图</td><td><a href='http://googleusercontent.com/maps.google.com/q=%.6f,%.6f' target='_blank'>🗺️ 查看地图</a></td></tr>"
                "</table>",
                loc.satellites,
                fabs(loc.latitude), lat_dir, 
                fabs(loc.longitude), lon_dir, 
                loc.speed, 
                loc.altitude,
                loc.latitude, loc.longitude); 
        } else {
            // 3. 没定位成功时，不再发 AT 指令，而是直接看 GGA 报文有没有抓到卫星
            if (loc.satellites > 0) {
                snprintf(msg_buf, sizeof(msg_buf), "⚠️ 已发现 %d 颗卫星，等待信号增强以获取有效坐标...(可能需要1-3分钟)", loc.satellites);
            } else {
                strcpy(msg_buf, "🛰️ GNSS 启动成功，搜星中... (请确保天线已接好并置于窗外或室外空旷处)");
            }
        }
    }
} else if (strcmp(type, "chipinfo") == 0) {
        char vbat[32] = "未知", temp[32] = "未知";
        
        // 1. 查询电压
        memset(resp_buf, 0, sizeof(resp_buf));
        if (modem_send_at_command("AT+MCHIPINFO=\"vbat\"", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, "+MCHIPINFO:");
            if (p) {
                // 查找第一个逗号后的内容
                char *val = strchr(p, ',');
                if (val) snprintf(vbat, sizeof(vbat), "%.3f V", atoi(val + 1) / 1000.0);
            }
        }
        
        // 2. 查询温度
        memset(resp_buf, 0, sizeof(resp_buf));
        if (modem_send_at_command("AT+MCHIPINFO=\"temp\"", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, "+MCHIPINFO:");
            if (p) {
                char *val = strchr(p, ',');
                if (val) snprintf(temp, sizeof(temp), "%d °C", atoi(val + 1));
            }
        }

        // 3. 结果判断
        if (strcmp(vbat, "未知") != 0 || strcmp(temp, "未知") != 0) {
            success = true;
            snprintf(msg_buf, sizeof(msg_buf), 
                "<table class='info-table'>"
                "<tr><td>Core Power (VBAT)</td><td>%s</td></tr>"
                "<tr><td>Chip Temp (TEMP)</td><td>%s</td></tr>"
                "</table>", vbat, temp);
        } else {
            strcpy(msg_buf, "❌ 溫壓讀取失敗 (模组可能不支持此指令)");
        }
    } else strcpy(msg_buf, "暂不支持此查询类型");

    char escaped_msg[2048] = {0};
    escape_json_string(msg_buf, escaped_msg, sizeof(escaped_msg));
    snprintf(json_buf, sizeof(json_buf), "{\"success\":%s,\"message\":\"%s\"}", success ? "true" : "false", escaped_msg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleFlightMode(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    char action[32] = {0};
    char query[WEB_SERVER_MAX_QUERY_BUFFER];
    if (read_url_query(req, query, sizeof(query))) {
        httpd_query_key_value(query, "action", action, sizeof(action));
    }
    char resp_buf[128] = {0}; char json[512] = {0};
    strcpy(json, "{\"success\":false,\"message\":\"查询失败\"}");
    if (strcmp(action, "query") == 0) {
        if (modem_send_at_command("AT+CFUN?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, "+CFUN:");
            if (p) {
                int mode = atoi(p + 6); char *modeStr="未知模式", *icon="❓";
                if(mode==0){ modeStr="最小功能模式"; icon="🔴"; } else if(mode==1){ modeStr="全功能模式"; icon="🟢"; } else if(mode==4){ modeStr="飞行模式"; icon="✈️"; }
                snprintf(json, sizeof(json), "{\"success\":true,\"message\":\"<table class='info-table'><tr><td>當前狀態</td><td>%s %s</td></tr><tr><td>CFUN值</td><td>%d</td></tr></table>\"}", icon, modeStr, mode);
            } else snprintf(json, sizeof(json), "{\"success\":false,\"message\":\"查询失败\"}");
        }
    } else if (strcmp(action, "toggle") == 0) {
        if (modem_send_at_command("AT+CFUN?", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *p = strstr(resp_buf, "+CFUN:");
            if (p) {
                int newMode = (atoi(p + 6) == 1) ? 4 : 1; char cmd[32]; snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", newMode);
                if (modem_send_at_command(cmd, resp_buf, sizeof(resp_buf), 5000) == ESP_OK && strstr(resp_buf, "OK")) {
                    snprintf(json, sizeof(json), "{\"success\":true,\"message\":\"%s\"}", (newMode==4) ? "已开启飞行模式 ✈️" : "已关闭飞行模式 🟢");
                } else snprintf(json, sizeof(json), "{\"success\":false,\"message\":\"切换失败\"}");
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleCops(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);
    char resp_buf[256] = {0};
    esp_err_t err = modem_send_at_command("AT+COPS?", resp_buf, sizeof(resp_buf), 5000);
    char json[512];
    for(int i=0; resp_buf[i]; i++) { if(resp_buf[i]=='\n') resp_buf[i]=' '; if(resp_buf[i]=='\r') resp_buf[i]=' '; }
    snprintf(json, sizeof(json), "{\"success\":%s,\"message\":\"%s\"}", err == ESP_OK ? "true":"false", resp_buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleATCommand(httpd_req_t *req) {
    if (!check_cookie_auth(req)) return httpd_resp_send_401(req);

    char cmd[128] = {0};

    if (req->method == HTTP_POST) {
        char body[WEB_SERVER_MAX_JSON_BODY] = {0};
        if (read_request_body_safe(req, body, sizeof(body))) {
            json_get_string_value(body, "cmd", cmd, sizeof(cmd));
        }
    } else {
        char query[WEB_SERVER_MAX_QUERY_BUFFER];
        if (read_url_query(req, query, sizeof(query))) {
            httpd_query_key_value(query, "cmd", cmd, sizeof(cmd));
            url_decode(cmd, cmd);
        }
    }

    if (cmd[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"缺少 AT 指令\"}", HTTPD_RESP_USE_STRLEN);
    }

    char resp_buf[1024] = {0};
    char json[2048] = {0};
    char escaped[2048] = {0};

    esp_err_t err = modem_send_at_command(cmd, resp_buf, sizeof(resp_buf), 10000);

    if (err == ESP_OK) {
        escape_json_string(resp_buf, escaped, sizeof(escaped));
        snprintf(json, sizeof(json), "{\"success\":true,\"message\":\"%s\"}", escaped);
    } else {
        snprintf(json, sizeof(json), "{\"success\":false,\"message\":\"指令执行超时或出错\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// ================= 服务注册与启动 =================
#define REG_URI(path, meth, func) do { \
    httpd_uri_t uri = { .uri = path, .method = meth, .handler = func, .user_ctx = NULL }; \
    httpd_register_uri_handler(server, &uri); \
} while(0)

esp_err_t web_server_start(void) {
    if (server != NULL) return ESP_OK;

    if (api_config_mutex == NULL) {
        api_config_mutex = xSemaphoreCreateMutex();
        if (!api_config_mutex) {
            ESP_LOGE(TAG, "无法创建 API 配置互斥锁");
            return ESP_FAIL;
        }
    }

    snprintf(session_token, sizeof(session_token), "%08X%08X", (unsigned int)esp_random(), (unsigned int)esp_random());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 25;
    config.stack_size = 8192;
    config.max_open_sockets = 4;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        REG_URI("/login", HTTP_GET, handleLogin);
        REG_URI("/login", HTTP_POST, handleDoLogin);
        REG_URI("/logout", HTTP_GET, handleLogout);
        REG_URI("/", HTTP_GET, handleRoot);
        REG_URI("/tools", HTTP_GET, handleToolsPage);
        REG_URI("/sms", HTTP_GET, handleToolsPage); 
        REG_URI("/ota", HTTP_GET, handleOtaPage);
        
        REG_URI("/api/config", HTTP_GET, handleApiConfig);
        REG_URI("/api/sysinfo", HTTP_GET, handleApiSysInfo);
        REG_URI("/api/smsc", HTTP_GET, handleSmsc);
        REG_URI("/api/smsc", HTTP_POST, handleSmsc);
        REG_URI("/api/getimei", HTTP_GET, handleGetImei);
        REG_URI("/api/setimei", HTTP_POST, handleSetImei);
        REG_URI("/api/save", HTTP_POST, handleSave);
        REG_URI("/api/sendsms", HTTP_POST, handleSendSms);
        REG_URI("/api/ping", HTTP_POST, handlePing);
        REG_URI("/api/query", HTTP_GET, handleQuery);
        REG_URI("/api/flight", HTTP_GET, handleFlightMode);
        REG_URI("/api/at", HTTP_POST, handleATCommand);
        REG_URI("/api/at", HTTP_GET, handleATCommand);
        REG_URI("/api/cops", HTTP_GET, handleCops);
        REG_URI("/api/toggledata", HTTP_POST, handleToggleData);
        REG_URI("/api/reboot", HTTP_POST, handleReboot);
        REG_URI("/api/update", HTTP_POST, handleApiUpdate);
        REG_URI("/api/resetnetwork", HTTP_POST, handleResetNetwork);

        ESP_LOGI(TAG, "Web 服务已启动，端口: %d", config.server_port);
        return ESP_OK;
    }
    return ESP_FAIL;
}

void web_server_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}