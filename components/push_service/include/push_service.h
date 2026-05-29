#ifndef PUSH_SERVICE_H
#define PUSH_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

void push_service_init(void);
void push_service_send(const char* sender, const char* message, const char* timestamp);

#ifdef __cplusplus
}
#endif

#endif // PUSH_SERVICE_H
