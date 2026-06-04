#include "modem_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "log_service.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "MODEM_DRV";

// 定义事件基底
ESP_EVENT_DEFINE_BASE(MODEM_EVENT);

// ================= FreeRTOS 同步原语与缓存 =================
static QueueHandle_t modem_uart_queue;
static SemaphoreHandle_t at_mutex;
static EventGroupHandle_t at_event_group;

#define AT_FLAG_OK       BIT0
#define AT_FLAG_ERROR    BIT1
#define AT_FLAG_PROMPT   BIT2  // 收到 '>' 提示符 (用于发短信)

static gnss_location_t g_current_location = {0}; // 全局缓存最新的 GNSS 位置数据
static bool g_allow_modem_data_network = false;  // 全局开关，控制是否允许 Modem 数据网络连接
static volatile modem_net_status_t g_cellular_status = MODEM_NET_STATUS_UNKNOWN; // 全局缓存当前蜂窝网络注册状态
static char g_modem_model[32] = "UNKNOWN";       // 全局缓存模组型号字符串
static char g_modem_fw_version[64] = "UNKNOWN";  // 全局缓存模组固件版本字符串
static bool g_modem_has_gnss = false;            // 全局缓存模组是否支持 GNSS 功能
static bool g_gnss_is_on = false;// 当前 GNSS 是否处于开启状态

// 同步请求共享响应缓冲区 (受 at_mutex 保护)
static char* shared_resp_buf = NULL;
static size_t shared_resp_max_len = 0;
static size_t shared_resp_current_len = 0;

// ================= 异步任务队列结构 =================
#define MODEM_AT_COMMAND_MAX_RESPONSE 2048

typedef struct {
    char cmd[128];
    uint32_t timeout_ms;
    modem_at_cb_t callback;
    void* user_ctx;
} async_at_req_t;

static QueueHandle_t at_async_queue;

// URC 事件回调注册
typedef struct {
    modem_urc_handler_t handler;
    void* user_ctx;
} urc_handler_entry_t;

#define MAX_URC_HANDLERS 4
static urc_handler_entry_t urc_handlers[MAX_URC_HANDLERS];
static SemaphoreHandle_t urc_handler_mutex;

static bool pending_cmt = false;
static char pending_cmt_header[128] = {0};

static void dispatch_urc_to_handlers(int event_id, const char* payload) {
    if (!payload) return;
    if (xSemaphoreTake(urc_handler_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < MAX_URC_HANDLERS; i++) {
        if (urc_handlers[i].handler) {
            urc_handlers[i].handler(event_id, payload, urc_handlers[i].user_ctx);
        } else {
            // 因为数组是紧凑的，遇到 NULL 说明后面全空了，直接打断循环
            break; 
        }
    }
    xSemaphoreGive(urc_handler_mutex);
}

modem_net_status_t modem_get_cellular_status(void) {
    return g_cellular_status;
}

const char* modem_get_model(void) {
    return g_modem_model;
}

const char* modem_get_fw_version(void) {
    return g_modem_fw_version;
}

bool modem_has_gnss(void) {
    return g_modem_has_gnss;
}

esp_err_t modem_set_gnss_state(bool enable) {
    if (!g_modem_has_gnss) return ESP_ERR_NOT_SUPPORTED;
    
    // 如果当前状态已经和目标状态一致，直接瞬间返回成功
    if (g_gnss_is_on == enable) {
        return ESP_OK;
    }
    
    esp_err_t err;
    
    if (enable) {
        // 设置定位模式参数
        modem_send_at_command("AT+MGNSSLOC=1", NULL, 0, 1000);
        vTaskDelay(pdMS_TO_TICKS(200));
        
        // 使用 AT+MGNSS=2 启动 GNSS
        err = modem_send_at_command("AT+MGNSS=2", NULL, 0, 2000);
    } else {
        err = modem_send_at_command("AT+MGNSS=0", NULL, 0, 2000);
    }
    
    // 只有当 AT 指令真正执行成功时，才更新我们记录的状态
    if (err == ESP_OK) {
        g_gnss_is_on = enable;
    }
    
    return err;
}

bool modem_get_location(gnss_location_t* out_loc) {
    if (!out_loc) return false;
    
    // 拷贝全局缓存给调用者
    *out_loc = g_current_location;
    
    // 只有当定位状态为 'A' (Active) 且有卫星时，我们认为定位成功
    return g_current_location.is_valid;
}

// 安全提取 NMEA 逗号分隔的第 n 个字段 (index 从 0 开始)
static bool get_nmea_field(const char* nmea, int index, char* out_buf, size_t max_len) {
    const char* p = nmea;
    int current_idx = 0;
    
    while (current_idx < index) {
        p = strchr(p, ',');
        if (!p) return false;
        p++; // 跳过逗号
        current_idx++;
    }
    
    const char* end = strchr(p, ',');
    if (!end) end = p + strlen(p); // 最后一个字段可能没有逗号
    
    size_t len = end - p;
    if (len >= max_len) len = max_len - 1;
    
    if (len > 0) {
        strncpy(out_buf, p, len);
        out_buf[len] = '\0';
        return true;
    }
    return false; // 字段为空
}

// 简单的经纬度转换 (度分格式 -> 十进制度)
static double nmea_to_degrees(const char* val, const char* dir) {
    if (!val || strlen(val) < 4) return 0.0;
    
    // NMEA 格式: DDDMM.MMMMM (经度) 或 DDMM.MMMMM (纬度)
    char deg_str[4] = {0};
    int deg_len = (strchr(val, '.') - val == 4) ? 2 : 3; // 纬度是2位，经度是3位
    strncpy(deg_str, val, deg_len);
    
    double degrees = atof(deg_str);
    double minutes = atof(val + deg_len);
    double result = degrees + (minutes / 60.0);
    
    if (dir[0] == 'S' || dir[0] == 'W') {
        result = -result;
    }
    return result;
}

// ================= GNSS 异步 URC 解析回调 =================
static void gnss_urc_handler(int event_id, const char* payload, void* user_ctx) {
    // 安全检查，确保只处理 GNSS 事件
    if (event_id != MODEM_EVENT_GNSS_UPDATED || !payload) return;
    
    // 不管有没有前缀，只要找到 '$' 开头的 NMEA 语句本体就行
    const char* nmea = strchr(payload, '$');
    if (!nmea) return; 
    
    char field_buf[32];
    char dir_buf[4];

    // 解析 GGA (提供高度、卫星数)
    if (strncmp(nmea, "$GNGGA", 6) == 0 || strncmp(nmea, "$GPGGA", 6) == 0) {
        if (get_nmea_field(nmea, 2, field_buf, sizeof(field_buf)) && 
            get_nmea_field(nmea, 3, dir_buf, sizeof(dir_buf))) {
            g_current_location.latitude = nmea_to_degrees(field_buf, dir_buf);
        }
        if (get_nmea_field(nmea, 4, field_buf, sizeof(field_buf)) && 
            get_nmea_field(nmea, 5, dir_buf, sizeof(dir_buf))) {
            g_current_location.longitude = nmea_to_degrees(field_buf, dir_buf);
        }
        if (get_nmea_field(nmea, 7, field_buf, sizeof(field_buf))) {
            g_current_location.satellites = atoi(field_buf);
        }
        if (get_nmea_field(nmea, 9, field_buf, sizeof(field_buf))) {
            g_current_location.altitude = atof(field_buf);
        }
    } 
    // 解析 RMC (提供有效性、速度)
    else if (strncmp(nmea, "$GNRMC", 6) == 0 || strncmp(nmea, "$GPRMC", 6) == 0) {
        if (get_nmea_field(nmea, 2, field_buf, sizeof(field_buf))) {
            // 'A' 代表有效定位 (Active)，'V' 代表无效 (Void)
            g_current_location.is_valid = (field_buf[0] == 'A');
        }
        if (get_nmea_field(nmea, 7, field_buf, sizeof(field_buf))) {
            g_current_location.speed = atof(field_buf) * 1.852f;
        }
        // ==========================================
        // 🌟 利用 GNSS 自动对齐系统时间
        // ==========================================
        char time_buf[16] = {0};
        char date_buf[16] = {0};
        
        // 字段 1 是时间 (hhmmss.ss)，字段 9 是日期 (ddmmyy)
        // 注意：只有在定位有效 (is_valid == true) 时，GNSS 给出的时间才是完全可信的
        if (g_current_location.is_valid && 
            get_nmea_field(nmea, 1, time_buf, sizeof(time_buf)) && 
            get_nmea_field(nmea, 9, date_buf, sizeof(date_buf))) {
            
            struct tm tm_info = {0};

            // 解析时间 (时、分、秒) - 使用 %2d 强制每次只读2个字符，完美避开警告
            int h = 0, m = 0, s = 0;
            if (sscanf(time_buf, "%2d%2d%2d", &h, &m, &s) == 3) {
                tm_info.tm_hour = h;
                tm_info.tm_min = m;
                tm_info.tm_sec = s;
            }

            // 解析日期 (日、月、年)
            int day = 0, mon = 0, yr = 0;
            if (sscanf(date_buf, "%2d%2d%2d", &day, &mon, &yr) == 3) {
                tm_info.tm_mday = day;
                tm_info.tm_mon = mon - 1;      // tm_mon 范围是 0-11
                tm_info.tm_year = yr + 100;    // 2000年以后加 100
            }

            // 转换为 UNIX 时间戳 (GNSS 上报的永远是绝对的 UTC 时间)
            time_t utc_time = mktime(&tm_info);

            // 读取 ESP32 当前系统时间做对比
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            
            // 只在系统时间偏差超过 2 秒时才去强制更新，避免每秒钟都无意义地重写系统时钟
            if (abs((int)(tv_now.tv_sec - utc_time)) > 2) {
                struct timeval tv = { .tv_sec = utc_time, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "⏰ ESP32 系统时钟已通过 GNSS 自动对齐!");
            }
        }
    }
}

// ================= 网络状态 URC 解析回调 =================
static void cellular_status_urc_handler(int event_id, const char* payload, void* user_ctx) {
    if (event_id != MODEM_EVENT_NET_CHANGED || !payload) return;

    const char* ptr = NULL;
    if ((ptr = strstr(payload, "+CEREG:")) != NULL || (ptr = strstr(payload, "+CREG:")) != NULL) {
        ptr = strchr(ptr, ':');
        if (ptr) {
            ptr++; 
            while (*ptr == ' ') ptr++; 

            int val1 = -1, val2 = -1;
            int parsed = sscanf(ptr, "%d,%d", &val1, &val2);

            int stat = -1;
            if (parsed == 2) {
                stat = val2;
            } else if (parsed == 1) {
                stat = val1;
            }

            if (stat != -1) {
                switch (stat) {
                    case 0: g_cellular_status = MODEM_NET_STATUS_NOT_REGISTERED; break;
                    case 1: g_cellular_status = MODEM_NET_STATUS_REGISTERED_HOME; break;
                    case 2: g_cellular_status = MODEM_NET_STATUS_SEARCHING; break;
                    case 3: g_cellular_status = MODEM_NET_STATUS_DENIED; break;
                    case 5: g_cellular_status = MODEM_NET_STATUS_REGISTERED_ROAMING; break;
                    default: g_cellular_status = MODEM_NET_STATUS_UNKNOWN; break;
                }
                ESP_LOGI(TAG, "📡 全局网络状态已更新 -> %d", g_cellular_status);
            }
        }
    }
}

esp_err_t toggle_modem_data_network(bool enable) {
    g_allow_modem_data_network = enable;
    if (!enable) {
        ESP_LOGI(TAG, "已在逻辑层关闭 4G 数据业务...");
    } else {
        ESP_LOGI(TAG, "已允许 4G 数据业务...");
    }
    return ESP_OK;
}

bool is_modem_data_allowed(void) {
    return g_allow_modem_data_network;
}

// ================= URC (主动上报) 全局事件广播与分发 =================
static void process_urc_line(const char* line) {
    // 🌟 修复：必须同时支持拦截直通(+CMT) 和 存储拉取(+CMTI)
    if (strstr(line, "+CMT:") != NULL || strstr(line, "+CMTI:") != NULL) {
        ESP_LOGI(TAG, "💌 收到新短信通知，正在向全局广播...");
        dispatch_urc_to_handlers(MODEM_EVENT_SMS_RECEIVED, line);
        if (esp_event_post(MODEM_EVENT, MODEM_EVENT_SMS_RECEIVED, line, strlen(line) + 1, pdMS_TO_TICKS(10)) != ESP_OK) {
            ESP_LOGW(TAG, "事件队列满，丢弃 URC: MODEM_EVENT_SMS_RECEIVED");
        }
    } else if (strstr(line, "+CLIP:") != NULL) {
        ESP_LOGI(TAG, "☎️ 收到来电，正在向全局广播...");
        dispatch_urc_to_handlers(MODEM_EVENT_CALL_RINGING, line);
        if (esp_event_post(MODEM_EVENT, MODEM_EVENT_CALL_RINGING, line, strlen(line) + 1, pdMS_TO_TICKS(10)) != ESP_OK) {
            ESP_LOGW(TAG, "事件队列满，丢弃 URC: MODEM_EVENT_CALL_RINGING");
        }
    } else if (strstr(line, "+MPING:") != NULL) {
        ESP_LOGI(TAG, "🌐 收到异步 Ping 数据，正在向全局广播...");
        dispatch_urc_to_handlers(MODEM_EVENT_PING_REPORT, line);
        if (esp_event_post(MODEM_EVENT, MODEM_EVENT_PING_REPORT, line, strlen(line) + 1, pdMS_TO_TICKS(10)) != ESP_OK) {
            ESP_LOGW(TAG, "事件队列满，丢弃 URC: MODEM_EVENT_PING_REPORT");
        }
    } else if (strstr(line, "+CEREG:") != NULL || strstr(line, "+CREG:") != NULL) {
        ESP_LOGI(TAG, "📡 蜂窝网络状态变化，正在向全局广播...");
        dispatch_urc_to_handlers(MODEM_EVENT_NET_CHANGED, line);
        if (esp_event_post(MODEM_EVENT, MODEM_EVENT_NET_CHANGED, line, strlen(line) + 1, pdMS_TO_TICKS(10)) != ESP_OK) {
            ESP_LOGW(TAG, "事件队列满，丢弃 URC: MODEM_EVENT_NET_CHANGED");
        }
    } 
    // 统一处理所有 GNSS 相关的上报 (包括带前缀和不带前缀的)
    else if (strstr(line, "+MGNSS:") != NULL || 
             strncmp(line, "$GN", 3) == 0 || 
             strncmp(line, "$GP", 3) == 0 || 
             strncmp(line, "$BD", 3) == 0 || 
             strncmp(line, "$GL", 3) == 0) {
        dispatch_urc_to_handlers(MODEM_EVENT_GNSS_UPDATED, line); 
    }
}

// ================= UART 后台解析任务 (优先级最高) =================
static void modem_event_task(void *pvParameters) {
    uart_event_t event;
    static uint8_t dtmp[UART_BUF_SIZE];
    
    static char line_buf[1024];
    static int line_pos = 0;

    ESP_LOGI(TAG, "Modem 异步解析引擎已启动");

    for (;;) {
        if (xQueueReceive(modem_uart_queue, (void *)&event, portMAX_DELAY)) {
            if (event.type == UART_DATA) {
                int len = uart_read_bytes(MODEM_UART_NUM, dtmp, event.size, portMAX_DELAY);
                
                for (int i = 0; i < len; i++) {
                    char c = dtmp[i];
                    
                    if (c == '>') {
                        xEventGroupSetBits(at_event_group, AT_FLAG_PROMPT);
                        continue;
                    }

                    if (line_pos < sizeof(line_buf) - 1) {
                        line_buf[line_pos++] = c;
                    }

                    if (c == '\n') {
                        line_buf[line_pos] = '\0';
                        
                        // 1. 同步响应拷贝
                        if (shared_resp_buf != NULL && shared_resp_current_len < shared_resp_max_len) {
                            strncat(shared_resp_buf, line_buf, shared_resp_max_len - shared_resp_current_len - 1);
                            shared_resp_current_len = strlen(shared_resp_buf);
                        }

                        // 2. AT 指令结束标志检测
                        const char *trimmed = line_buf;
                        while (*trimmed == '\r' || *trimmed == '\n' || *trimmed == ' ' || *trimmed == '\t') trimmed++;
                        
                        if (strncmp(trimmed, "OK", 2) == 0 && (trimmed[2] == '\0' || trimmed[2] == '\r' || trimmed[2] == '\n')) {
                            xEventGroupSetBits(at_event_group, AT_FLAG_OK);
                        } else if (strncmp(trimmed, "ERROR", 5) == 0 || strstr(line_buf, "+CME ERROR:") != NULL) {
                            xEventGroupSetBits(at_event_group, AT_FLAG_ERROR);
                        }
                        // 3. 拦截主动上报 (URC) - 放行 '+' 和 '$' 开头的行
                        else if (line_buf[0] == '+' || line_buf[0] == '$') {
                            const char *trim_line = trimmed;
                            if (pending_cmt) {
                                pending_cmt = false;
                                pending_cmt_header[0] = '\0';
                            }
                            if (strncmp(trim_line, "+CMT:", 5) == 0) {
                                strncpy(pending_cmt_header, trim_line, sizeof(pending_cmt_header) - 1);
                                pending_cmt_header[sizeof(pending_cmt_header) - 1] = '\0';
                                pending_cmt = true;
                            } else {
                                // 转发给专门的路由函数进行判定
                                process_urc_line(trim_line);
                            }
                        } else if (pending_cmt) {
                            char combined[128 + 2048] = {0};
                            snprintf(combined, sizeof(combined), "%s\r\n%s", pending_cmt_header, line_buf);
                            process_urc_line(combined);
                            pending_cmt = false;
                            pending_cmt_header[0] = '\0';
                        }

                        line_pos = 0;
                    }
                }
            } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                ESP_LOGW(TAG, "UART 缓冲区溢出");
                uart_flush_input(MODEM_UART_NUM);
                xQueueReset(modem_uart_queue);
            }
        }
    }
    vTaskDelete(NULL);
}

// ================= API：同步发送指令并阻塞等待 =================
esp_err_t modem_send_at_command(const char* cmd, char* out_resp, size_t max_len, uint32_t timeout_ms) {
    if (xSemaphoreTake(at_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "获取 AT 互斥锁超时 (上一个指令耗时太长)");
        return ESP_ERR_TIMEOUT;
    }

    xEventGroupClearBits(at_event_group, AT_FLAG_OK | AT_FLAG_ERROR | AT_FLAG_PROMPT);
    if (out_resp != NULL && max_len > 0) {
        out_resp[0] = '\0';
        shared_resp_buf = out_resp;
        shared_resp_max_len = max_len;
        shared_resp_current_len = 0;
    } else {
        shared_resp_buf = NULL;
    }

    uart_flush_input(MODEM_UART_NUM);
    ESP_LOGI(TAG, "-> 发送: %s", cmd);
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

    EventBits_t bits = xEventGroupWaitBits(
        at_event_group,
        AT_FLAG_OK | AT_FLAG_ERROR,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms)
    );

    shared_resp_buf = NULL;
    xSemaphoreGive(at_mutex);

    if (bits & AT_FLAG_OK) {
        ESP_LOGI(TAG, "<- 响应: %s", (out_resp && strlen(out_resp) > 0) ? out_resp : "OK");
        return ESP_OK;
    } else if (bits & AT_FLAG_ERROR) {
        ESP_LOGE(TAG, "<- 响应: ERROR (指令: %s)", cmd);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "<- 指令超时: %s", cmd);
        return ESP_ERR_TIMEOUT;
    }
}

// ================= 后台异步执行 Worker 任务 =================
static void modem_at_worker_task(void *pvParameters) {
    async_at_req_t req;
    char resp_buf[MODEM_AT_COMMAND_MAX_RESPONSE];

    while (1) {
        if (xQueueReceive(at_async_queue, &req, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Worker 处理异步指令: %s", req.cmd);
            memset(resp_buf, 0, sizeof(resp_buf));
            
            esp_err_t err = modem_send_at_command(req.cmd, resp_buf, sizeof(resp_buf), req.timeout_ms);
            if (req.callback) {
                req.callback(err, resp_buf, req.user_ctx);
            }
        }
    }
}

esp_err_t modem_enqueue_at_command_async(const char* cmd, uint32_t timeout_ms, modem_at_cb_t cb, void* user_ctx) {
    if (!cmd || strlen(cmd) >= 128) return ESP_ERR_INVALID_ARG;
    async_at_req_t req;

    strncpy(req.cmd, cmd, sizeof(req.cmd) - 1);
    req.cmd[sizeof(req.cmd) - 1] = '\0';
    req.timeout_ms = timeout_ms;
    req.callback = cb;
    req.user_ctx = user_ctx;

    if (xQueueSend(at_async_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t modem_register_urc_handler(modem_urc_handler_t handler, void* user_ctx) {
    if (!handler) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(urc_handler_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t res = ESP_ERR_NO_MEM;
    for (int i = 0; i < MAX_URC_HANDLERS; i++) {
        if (urc_handlers[i].handler == NULL) {
            urc_handlers[i].handler = handler;
            urc_handlers[i].user_ctx = user_ctx;
            res = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(urc_handler_mutex);
    return res;
}

esp_err_t modem_unregister_urc_handler(modem_urc_handler_t handler, void* user_ctx) {
    if (!handler) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(urc_handler_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t res = ESP_ERR_NOT_FOUND;
    int found_idx = -1;

    for (int i = 0; i < MAX_URC_HANDLERS; i++) {
        if (urc_handlers[i].handler == handler && urc_handlers[i].user_ctx == user_ctx) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        for (int i = found_idx; i < MAX_URC_HANDLERS - 1; i++) {
            urc_handlers[i] = urc_handlers[i + 1]; 
        }
        urc_handlers[MAX_URC_HANDLERS - 1].handler = NULL;
        urc_handlers[MAX_URC_HANDLERS - 1].user_ctx = NULL;
        
        res = ESP_OK;
    }

    xSemaphoreGive(urc_handler_mutex);
    return res;
}

esp_err_t modem_wait_for_prompt(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(
        at_event_group,
        AT_FLAG_PROMPT | AT_FLAG_ERROR,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms)
    );
    if (bits & AT_FLAG_PROMPT) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

void modem_send_raw_data(const char* data) {
    uart_write_bytes(MODEM_UART_NUM, data, strlen(data));
}

static esp_err_t modem_send_at_command_no_wait(const char* cmd) {
    if (xSemaphoreTake(at_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
    xSemaphoreGive(at_mutex);
    return ESP_OK;
}

static esp_err_t modem_wait_for_final_response(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(
        at_event_group,
        AT_FLAG_OK | AT_FLAG_ERROR,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms)
    );
    if (bits & AT_FLAG_OK) return ESP_OK;
    if (bits & AT_FLAG_ERROR) return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

#define MODEM_PING_DONE_BIT BIT0

typedef struct {
    EventGroupHandle_t done_group;
    int count;
    int total_rtt;
    char details[512];
} modem_ping_result_t;

static void modem_ping_urc_handler(int event_id, const char* payload, void* user_ctx) {
    if (event_id != MODEM_EVENT_PING_REPORT || !payload || !user_ctx) return;
    modem_ping_result_t *result = (modem_ping_result_t*) user_ctx;
    if (strstr(payload, "+MPING:") == NULL || strstr(payload, "STAT") != NULL) return;

    int rtt = -1;
    char *last_comma = strrchr((char*)payload, ',');
    if (last_comma) {
        char *prev_comma = NULL;
        for (char *p = (char*)payload; p < last_comma; p++) {
            if (*p == ',') prev_comma = p;
        }
        if (prev_comma) {
            rtt = atoi(prev_comma + 1);
        } else {
            rtt = atoi(last_comma + 1);
        }
    }
    if (rtt < 0) return;

    if (result->count < 255) {
        result->count++;
    }
    result->total_rtt += rtt;
    char temp[64];
    snprintf(temp, sizeof(temp), "Ping %d: %d ms\n", result->count, rtt);
    strncat(result->details, temp, sizeof(result->details) - strlen(result->details) - 1);

    if (result->count >= 4) {
        xEventGroupSetBits(result->done_group, MODEM_PING_DONE_BIT);
    }
}

esp_err_t modem_ping(const char* target, int count, int timeout_s, int* out_success_count, int* out_avg_rtt, char* details, size_t details_len) {
    if (!target || strlen(target) == 0 || count <= 0 || timeout_s <= 0) return ESP_ERR_INVALID_ARG;

    if (out_success_count) *out_success_count = 0;
    if (out_avg_rtt) *out_avg_rtt = 0;
    if (details && details_len > 0) details[0] = '\0';

    EventGroupHandle_t group = xEventGroupCreate();
    if (!group) return ESP_ERR_NO_MEM;

    modem_ping_result_t result = {
        .done_group = group,
        .count = 0,
        .total_rtt = 0,
        .details = {0}
    };

    esp_err_t reg_err = modem_register_urc_handler(modem_ping_urc_handler, &result);
    if (reg_err != ESP_OK) {
        vEventGroupDelete(group);
        return reg_err;
    }

    char dummy[128] = {0};
    char cmd[128] = {0};
    esp_err_t err = ESP_FAIL;
    snprintf(cmd, sizeof(cmd), "AT+MPING=\"%s\",%d,%d", target, timeout_s, count);
    err = modem_send_at_command(cmd, dummy, sizeof(dummy), 20000);
    if (err != ESP_OK) {
        dummy[0] = '\0';
        snprintf(cmd, sizeof(cmd), "AT+MPING=\"%s\",%d,%d", target, count, timeout_s);
        err = modem_send_at_command(cmd, dummy, sizeof(dummy), 20000);
    }

    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)count * timeout_s * 1000 + 2000);
    xEventGroupWaitBits(group, MODEM_PING_DONE_BIT, pdTRUE, pdFALSE, wait_ticks);

    modem_unregister_urc_handler(modem_ping_urc_handler, &result);
    vEventGroupDelete(group);

    if (out_success_count) *out_success_count = result.count;
    if (result.count > 0 && out_avg_rtt) {
        *out_avg_rtt = result.total_rtt / result.count;
    }
    if (details && details_len > 0) {
        strncpy(details, result.details, details_len - 1);
        details[details_len - 1] = '\0';
    }
    return err;
}

esp_err_t modem_get_smsc(char* smsc, size_t max_len) {
    if (smsc == NULL || max_len == 0) return ESP_ERR_INVALID_ARG;
    char resp_buf[256] = {0};
    if (modem_send_at_command("AT+CSCA?", resp_buf, sizeof(resp_buf), 2000) != ESP_OK) {
        return ESP_FAIL;
    }
    char* start = strstr(resp_buf, "+CSCA:");
    if (!start) return ESP_FAIL;
    start = strchr(start, '"');
    if (!start) return ESP_FAIL;
    start++;
    char* end = strchr(start, '"');
    if (!end) return ESP_FAIL;
    size_t len = end - start;
    if (len >= max_len) len = max_len - 1;
    strncpy(smsc, start, len);
    smsc[len] = '\0';
    return ESP_OK;
}

esp_err_t modem_set_smsc(const char* smsc) {
    if (smsc == NULL) return ESP_ERR_INVALID_ARG;
    char cmd[64];
    if (strlen(smsc) == 0) {
        ESP_LOGI(TAG, "正在恢复模组默认 SMSC");
        snprintf(cmd, sizeof(cmd), "AT+CSCA=\"\"");
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CSCA=\"%s\"", smsc);
    }

    char resp_buf[256] = {0};
    if (modem_send_at_command(cmd, resp_buf, sizeof(resp_buf), 3000) != ESP_OK) {
        return ESP_FAIL;
    }
    return (strstr(resp_buf, "OK") != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t modem_send_sms_text(const char* phone, const char* content, uint32_t timeout_ms) {
    if (phone == NULL || content == NULL) return ESP_ERR_INVALID_ARG;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", phone);

    if (modem_send_at_command("AT+CMGF=1", NULL, 0, 2000) != ESP_OK) {
        return ESP_FAIL;
    }
    xEventGroupClearBits(at_event_group, AT_FLAG_OK | AT_FLAG_ERROR | AT_FLAG_PROMPT);
    if (modem_send_at_command_no_wait(cmd) != ESP_OK) {
        modem_send_at_command("AT+CMGF=0", NULL, 0, 1000);
        return ESP_FAIL;
    }
    if (modem_wait_for_prompt(timeout_ms) != ESP_OK) {
        modem_send_at_command("AT+CMGF=0", NULL, 0, 1000);
        return ESP_ERR_TIMEOUT;
    }

    modem_send_raw_data(content);
    modem_send_raw_data("\x1A");

    esp_err_t result = modem_wait_for_final_response(timeout_ms);
    modem_send_at_command("AT+CMGF=0", NULL, 0, 1000);
    return result;
}

esp_err_t modem_read_sms_pdu(int index, char* out_pdu, size_t max_len) {
    if (!out_pdu || max_len == 0) return ESP_ERR_INVALID_ARG;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);

    char resp_buf[1024] = {0};
    esp_err_t err = modem_send_at_command(cmd, resp_buf, sizeof(resp_buf), 3000);
    if (err != ESP_OK) {
        return err;
    }

    // 解析格式通常为:
    // +CMGR: 1,,24\r\n
    // 0891683108100005F0040D916831...\r\n
    // OK
    char* header = strstr(resp_buf, "+CMGR:");
    if (!header) {
        return ESP_ERR_NOT_FOUND; // 短信槽位为空或已被删除
    }

    // 跳到下一行寻找 PDU 数据
    char* pdu_start = strchr(header, '\n');
    if (!pdu_start) {
        return ESP_FAIL;
    }
    pdu_start++; 

    // 跳过多余的不可见控制字符
    while (*pdu_start == '\r' || *pdu_start == '\n' || *pdu_start == ' ') {
        pdu_start++;
    }

    // PDU 直到遇到下一个换行符为止
    char* pdu_end = pdu_start;
    while (*pdu_end != '\0' && *pdu_end != '\r' && *pdu_end != '\n') {
        pdu_end++;
    }

    size_t pdu_len = pdu_end - pdu_start;
    if (pdu_len == 0 || pdu_len >= max_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(out_pdu, pdu_start, pdu_len);
    out_pdu[pdu_len] = '\0';

    return ESP_OK;
}

esp_err_t modem_delete_sms(int index) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    return modem_send_at_command(cmd, NULL, 0, 2000);
}

esp_err_t modem_get_all_sms_indices(int* out_indices, int max_count, int* out_count) {
    if (!out_indices || !out_count || max_count <= 0) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    // 由于 AT+CMGL=4 可能会返回多条短信 PDU，使用固定静态缓冲区避免运行时分配
    static char resp_buf[4096];
    memset(resp_buf, 0, sizeof(resp_buf));

    // AT+CMGL=4 代表列出所有 (已读、未读、未发送等)
    esp_err_t err = modem_send_at_command("AT+CMGL=4", resp_buf, sizeof(resp_buf), 5000);
    if (err == ESP_OK) {
        char* p = resp_buf;
        // 寻找所有 "+CMGL:" 开头的行
        while ((p = strstr(p, "+CMGL:")) != NULL && *out_count < max_count) {
            p += 6; // 跳过 "+CMGL:" 字符
            while (*p == ' ') p++; // 跳过可能存在的空格
            
            int idx = atoi(p); // 提取逗号前的索引数字
            if (idx >= 0) {
                out_indices[*out_count] = idx;
                (*out_count)++;
            }
        }
    }
    return err;
}

// ================= 驱动初始化 =================
esp_err_t modem_driver_init(void) {
    at_mutex = xSemaphoreCreateMutex();
    at_event_group = xEventGroupCreate();
    urc_handler_mutex = xSemaphoreCreateMutex();
    at_async_queue = xQueueCreate(8, sizeof(async_at_req_t));
    if (!at_mutex || !at_event_group || !urc_handler_mutex || !at_async_queue) {
        ESP_LOGE(TAG, "初始化 Modem 同步资源失败");
        if (at_mutex) vSemaphoreDelete(at_mutex);
        if (at_event_group) vEventGroupDelete(at_event_group);
        if (urc_handler_mutex) vSemaphoreDelete(urc_handler_mutex);
        if (at_async_queue) vQueueDelete(at_async_queue);
        return ESP_FAIL;
    }

    modem_register_urc_handler(cellular_status_urc_handler, NULL);

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
        vSemaphoreDelete(at_mutex);
        vEventGroupDelete(at_event_group);
        vSemaphoreDelete(urc_handler_mutex);
        vQueueDelete(at_async_queue);
        return err;
    }

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(MODEM_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_UART_NUM, MODEM_TXD_PIN, MODEM_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(MODEM_UART_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 20, &modem_uart_queue, 0));

    gpio_reset_pin(MODEM_EN_PIN);
    gpio_set_direction(MODEM_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MODEM_EN_PIN, 0); 
    vTaskDelay(pdMS_TO_TICKS(1200));
    gpio_set_level(MODEM_EN_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(3000)); 

    xTaskCreate(modem_event_task, "modem_event_task", 4096, NULL, 6, NULL);
    xTaskCreate(modem_at_worker_task, "modem_at_worker", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "正在与模组握手并查询型号...");
    
    bool modem_ready = false;
    for (int i = 0; i < 100; i++) {
        if (modem_send_at_command("AT", NULL, 0, 3000) == ESP_OK) {
            modem_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (modem_ready) {
        modem_send_at_command("ATE0", NULL, 0, 1000);
        
        char resp_buf[128] = {0};
        
        if (modem_send_at_command("AT+CGMM", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *start = resp_buf;
            while (*start == '\r' || *start == '\n' || *start == ' ') start++; 
            char *end = start;
            while (*end != '\r' && *end != '\n' && *end != '\0') end++; 
            
            size_t len = end - start;
            if (len > 0 && len < sizeof(g_modem_model)) {
                strncpy(g_modem_model, start, len);
                g_modem_model[len] = '\0';
                ESP_LOGI(TAG, "📦 获取到模组型号: %s", g_modem_model);
            }
        }

        memset(resp_buf, 0, sizeof(resp_buf));
        if (modem_send_at_command("AT+CGMR", resp_buf, sizeof(resp_buf), 2000) == ESP_OK) {
            char *start = resp_buf;
            while (*start == '\r' || *start == '\n' || *start == ' ') start++; 
            char *end = start;
            while (*end != '\r' && *end != '\n' && *end != '\0') end++; 
            
            size_t len = end - start;
            if (len > 0 && len < sizeof(g_modem_fw_version)) {
                strncpy(g_modem_fw_version, start, len);
                g_modem_fw_version[len] = '\0';
                ESP_LOGI(TAG, "🔖 获取到固件版本: %s", g_modem_fw_version);
                
                if (strstr(g_modem_fw_version, "-GSLN") != NULL) {
                    g_modem_has_gnss = true;
                    ESP_LOGI(TAG, "🛰️ 模组支持 GNSS 功能");
                } else {
                    g_modem_has_gnss = false;
                    ESP_LOGI(TAG, "🚫 模组不支持 GNSS 功能");
                }
            }
        }
        
        modem_send_at_command("AT+CEREG=1", NULL, 0, 2000);
        modem_send_at_command("AT+CREG=1", NULL, 0, 2000);
        
    } else {
        ESP_LOGE(TAG, "❌ 模组无响应，初始化型号失败！");
        return ESP_FAIL;
    }

    if (g_modem_has_gnss) {
        modem_register_urc_handler(gnss_urc_handler, NULL);
        ESP_LOGI(TAG, "🚀 正在后台启动 GNSS 引擎...");
        modem_set_gnss_state(true); 
    }

    return ESP_OK;
}