#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PUSH_CHANNELS 5
#define MAX_CRON_TASKS 3

// 推送通道类型枚举
typedef enum {
    PUSH_TYPE_NONE = 0,
    PUSH_TYPE_POST_JSON = 1,
    PUSH_TYPE_BARK = 2,
    PUSH_TYPE_GET = 3,
    PUSH_TYPE_SERVERCHAN = 6,
    PUSH_TYPE_CUSTOM = 7,
    PUSH_TYPE_GOTIFY = 9,
    PUSH_TYPE_TELEGRAM = 10
} push_type_t;

// 推送通道结构体
typedef struct {
    bool enabled;
    push_type_t type;
    char name[32];
    char url[128];
    char key1[64];
    char key2[64];
    char customBody[256];
} push_channel_t;

// 定时任务结构体
typedef struct {
    bool enabled;
    int type;                  // 0: Ping, 1: SMS
    int hour;
    int minute;
    char phone[20];
    char content[128];
    char pingTarget[64];
    int daysInterval;          // 執行間隔天數 (1代表每天，0代表單次任務)
    unsigned long nextRunEpoch; // 下一次執行的絕對 Unix 時間戳 (秒)
} cron_task_t;

// 全局应用配置结构体
typedef struct {
    char adminPhone[20];
    char webUser[32];
    char webPass[64];
    char numberBlackList[256];
    
    bool syslogEnabled;
    bool callNotifyEnabled;
    char syslogServer[64];
    int  syslogPort;
    
    char plmn[10];
    char smsc[32];
    char imei[20];
    
    push_channel_t pushChannels[MAX_PUSH_CHANNELS];
    cron_task_t cronTasks[MAX_CRON_TASKS];
} app_config_t;

// 暴露给其他模块的全局配置实例
extern app_config_t g_app_config;

// API 声明
/**
 * @brief 初始化配置管理器
 * @note 这个函数会在系统启动时被调用，负责加载配置文件（如果存在）并将其解析到全局配置结构体中。请确保在调用其他任何依赖配置的函数之前先调用这个初始化函数。
 * @note 配置文件的存储格式可以是 JSON、INI 或者你喜欢的任何格式，只要在实现中正确解析并填充到 g_app_config 中即可。你也可以在这个函数中设置一些默认值，以防配置文件缺失或损坏时系统仍能正常运行。
 * @note 该函数内部会调用 config_load_all() 来加载配置文件，如果你需要在运行时动态修改配置并保存，可以调用 config_save_all() 将当前的 g_app_config 写回到存储中。
 * @note 请根据你的存储介质（例如 SPIFFS、NVS 或 SD 卡）来实现 config_load_all() 和 config_save_all() 函数，以确保配置数据能够正确持久化。
 */
void config_manager_init(void);
/**
 * @brief 加载配置文件并解析到全局配置结构体
 * @note 该函数会被 config_manager_init() 调用来加载配置文件，你也可以在运行时需要重新加载配置时调用它。请根据你的存储介质来实现这个函数，确保能够正确读取配置数据并填充到 g_app_config 中。
 */
void config_load_all(void);
/**
 * @brief 保存全局配置结构体到配置文件
 * @note 该函数会将当前的 g_app_config 写回到存储中，以确保配置的持久化。请根据你的存储介质来实现这个函数。
 */
void config_save_all(void);

#endif // CONFIG_MANAGER_H