#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

// 启动 Web 服务器 (监听 80 端口)
esp_err_t web_server_start(void);

// 停止 Web 服务器 (通常用于 OTA 期间)
void web_server_stop(void);

#endif // WEB_SERVER_H