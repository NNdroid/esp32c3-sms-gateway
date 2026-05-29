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
void config_manager_init(void);
void config_load_all(void);
void config_save_all(void);

#endif // CONFIG_MANAGER_H