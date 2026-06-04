#ifndef CALL_PROCESSOR_H
#define CALL_PROCESSOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化来电处理引擎
 * @note 必须在 modem_driver_init 之后调用，以确保 URC 事件队列和任务准备就绪
 * @note 该函数会注册来电相关的 URC 处理函数，并创建必要的 FreeRTOS 资源（例如消息队列、事件组等）。当收到来电相关的 URC 上报时，处理函数会解析来电信息并将事件发布到系统中，供其他组件订阅和处理。请确保在调用 modem_driver_init() 完成初始化后再调用该函数，以确保获取到正确的结果。
 * @note 目前来电处理引擎的功能较为基础，主要用于检测来电事件并获取来电号码。后续可以根据需要扩展更多功能，例如自动接听、拒接、挂断、来电转发等。请根据你的业务需求和模组支持的 AT 指令集进行相应的开发和测试。
 */
void call_processor_init(void);

#ifdef __cplusplus
}
#endif

#endif // CALL_PROCESSOR_H
