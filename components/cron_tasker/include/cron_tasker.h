#ifndef CRON_TASKER_H
#define CRON_TASKER_H

#include "esp_err.h"

/**
 * @brief 初始化定时任务管理器，启动后台常驻线程
 */
esp_err_t cron_tasker_init(void);

#endif // CRON_TASKER_H