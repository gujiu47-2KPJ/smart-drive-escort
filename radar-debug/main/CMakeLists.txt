idf_component_register(
    # 一次只能编译一个主程序：radar_data_main.c 和 radar_uart_main.c
    # 都定义了 app_main，一起编译会 duplicate symbol。
    #   radar_data_main.c : UART 数据读取（当前使用，通信已实测打通）
    #   radar_uart_main.c : UART 抓包诊断版（排查时用，改这一行切换）
    SRCS "radar_data_main.c"
    INCLUDE_DIRS "."
)
