# 准备一个空的源文件列表变量
set(COMPONENT_SRCS "")

# 如果 Kconfig 中开启了功能，就把 .c 文件塞进列表
if(CONFIG_ENABLE_CALL_PROCESSOR)
    set(COMPONENT_SRCS "call_processor.c")
    # 如果有多个文件，可以写成: set(COMPONENT_SRCS "file1.c" "file2.c")
endif()

# 统一注册！
# 如果没开启，SRCS就是空的，它自动变成合法的纯头文件库。
# 如果开启了，SRCS里就有文件，它正常编译，完美！
idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES modem_driver push_service
)