#include "log_service.h"
#include "config_manager.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "esp_netif.h"

#define SYSLOG_MAX_MESSAGE 384
#define SYSLOG_HOSTNAME_MAX 64

static bool log_service_forwarding_ready = false;

static bool log_service_should_forward(void) {
    return log_service_forwarding_ready && g_app_config.syslogEnabled && g_app_config.syslogServer[0] != '\0' && g_app_config.syslogPort > 0;
}

static int log_service_severity(int level) {
    switch (level) {
        case ESP_LOG_ERROR: return 3;
        case ESP_LOG_WARN: return 4;
        case ESP_LOG_INFO: return 6;
        case ESP_LOG_DEBUG: return 7;
        case ESP_LOG_VERBOSE: return 7;
        default: return 6;
    }
}

static void log_service_build_timestamp(char* buffer, size_t len) {
    time_t now = time(NULL);
    if (now <= 0) {
        strncpy(buffer, "-", len);
        buffer[len - 1] = '\0';
        return;
    }

    struct tm timeinfo;
    if (gmtime_r(&now, &timeinfo) == NULL) {
        strncpy(buffer, "-", len);
        buffer[len - 1] = '\0';
        return;
    }

    strftime(buffer, len, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
}

static void log_service_get_hostname(char* out, size_t len) {
    if (len == 0) {
        return;
    }
    if (gethostname(out, len) == 0) {
        out[len - 1] = '\0';
        return;
    }
    strncpy(out, "-", len);
    out[len - 1] = '\0';
}

static void log_service_get_ipaddr(char* out, size_t len) {
    if (len == 0) {
        return;
    }

    out[0] = '\0';
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        strncpy(out, "-", len);
        out[len - 1] = '\0';
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        strncpy(out, "-", len);
        out[len - 1] = '\0';
        return;
    }

    if (inet_ntop(AF_INET, &ip_info.ip, out, len) == NULL) {
        strncpy(out, "-", len);
        out[len - 1] = '\0';
    }
}

static void log_service_build_syslog_payload(int level, const char* tag, const char* message, char* out, size_t out_len) {
    char timestamp[32];
    char hostname[SYSLOG_HOSTNAME_MAX];
    char ip_address[32];

    log_service_build_timestamp(timestamp, sizeof(timestamp));
    log_service_get_hostname(hostname, sizeof(hostname));
    log_service_get_ipaddr(ip_address, sizeof(ip_address));

    int pri = 16 + log_service_severity(level);
    snprintf(out, out_len, "<%d>1 %s %s %s %s - %s: %s",
             pri,
             timestamp,
             hostname,
             ip_address,
             hostname,
             tag,
             message);
}

static bool log_service_send_udp(const char* server, int port, const char* payload) {
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    char port_str[8];
    int sock = -1;
    bool success = false;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(server, port_str, &hints, &result) != 0 || result == NULL) {
        return false;
    }

    sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(result);
        return false;
    }

    ssize_t sent = sendto(sock, payload, strlen(payload), 0, result->ai_addr, result->ai_addrlen);
    if (sent == (ssize_t)strlen(payload)) {
        success = true;
    }

    close(sock);
    freeaddrinfo(result);
    return success;
}

static void log_service_vsyslog(int level, const char* tag, const char* format, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);

    char* message = malloc(SYSLOG_MAX_MESSAGE);
    if (message == NULL) {
        va_end(ap_copy);
        return;
    }
    vsnprintf(message, SYSLOG_MAX_MESSAGE, format, ap_copy);
    va_end(ap_copy);

    size_t payload_size = SYSLOG_MAX_MESSAGE + 128;
    char* payload = malloc(payload_size);
    if (payload == NULL) {
        free(message);
        return;
    }

    log_service_build_syslog_payload(level, tag, message, payload, payload_size);
    log_service_send_udp(g_app_config.syslogServer, g_app_config.syslogPort, payload);

    free(payload);
    free(message);
}

void log_service_init(void) {
    log_service_forwarding_ready = false;
}

void log_service_set_ready(bool ready) {
    log_service_forwarding_ready = ready;
}

void log_service_voutput(int level, const char* tag, const char* format, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);

    // 创建带换行符的新格式字符串
    char new_format[strlen(format) + 3];
    snprintf(new_format, sizeof(new_format), "%s\r\n", format);

    esp_log_va(ESP_LOG_CONFIG_INIT(level), tag, new_format, ap);

    if (log_service_should_forward()) {
        log_service_vsyslog(level, tag, format, ap_copy);
    }

    va_end(ap_copy);
}