#ifndef PUSH_SERVICE_H
#define PUSH_SERVICE_H

#define NET_READY_BIT BIT0

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化推送服务，创建后台任务和队列
 * @note 该函数必须在系统启动时调用一次，建议在 main.c 的 app_main 函数中调用。初始化完成后，系统将创建一个后台任务持续监听推送队列，并在有新消息时自动处理推送逻辑。
 * @warning 请确保在调用该函数之前已经完成了必要的系统初始化（例如 NVS、网络等），以避免推送服务无法正常工作。
 */
void push_service_init(void);
/**
 * @brief 发送推送消息到后台队列
 * @param sender 消息发送者（例如电话号码或标识符）
 * @param message 消息内容字符串
 * @param timestamp 消息时间戳字符串（格式建议为 "YYYY-MM-DD HH:MM:SS"）
 * @note 该函数会将消息封装成一个结构体并尝试快速压入队列，如果队列已满则会丢弃该消息并记录错误日志。调用者无需等待推送完成，函数会立即返回。后台任务会异步处理队列中的消息并执行实际的推送操作。
 */
void push_service_send(const char* sender, const char* message, const char* timestamp);
/**
 * @brief 立即发送推送消息（带自动时间戳）
 * @param sender 消息发送者（例如电话号码或标识符）
 * @param message 消息内容字符串
 * @note 该函数会自动获取当前系统时间并格式化为字符串，然后调用 push_service_send 将消息压入队列。适用于需要快速发送但不关心精确时间戳的场景。
 * @warning 该函数会调用 time() 和 localtime_r() 获取系统时间，确保系统时间已正确同步（例如通过 NTP）以获得准确的时间戳。
 */
void push_service_send_now(const char* sender, const char* message);
/**
 * @brief 发送推送消息并等待确认（高级接口，带短信索引）
 * @param sender 消息发送者（例如电话号码或标识符）
 * @param message 消息内容字符串
 * @param timestamp 消息时间戳字符串（格式建议为 "YYYY-MM-DD HH:MM:SS"）
 * @param indexes 短信索引数组，包含与该消息相关的短信在模组中的槽位索引
 * @param index_count 短信索引数组的长度
 * @return true 如果推送成功并收到服务器确认，false 如果推送失败或服务器拒绝
 * @note 该函数会尝试将消息发送到所有已启用的推送通道，并等待每个通道的推送结果。如果所有通道至少有一个成功，则返回 true；如果所有通道都失败或被拒绝，则返回 false。该函数适用于需要确保消息被成功推送到服务器的场景，但会增加调用的复杂度和等待时间。
 * @warning 该函数会阻塞调用线程直到收到推送结果，确保在调用时不处于高优先级或时间敏感的上下文中，以避免影响系统的响应性。
 */
void push_service_send_with_ack(const char* sender, const char* message, const char* timestamp, const int* indexes, int index_count);

#ifdef __cplusplus
}
#endif

#endif // PUSH_SERVICE_H
