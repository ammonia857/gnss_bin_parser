cmake_minimum_required(VERSION 3.16)
project(gnss_bin_parser VERSION 1.0.0 LANGUAGES CXX)

# C++17标准，严格编译
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 跨平台编译选项
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 头文件路径
include_directories(${PROJECT_SOURCE_DIR}/include)

# ============================================================
# gnss_struct层：GNSS数据结构体（时间、观测、可见性、定位）
# ============================================================
set(GNSS_STRUCT_SOURCES
    src/gnss_struct/gnss_time.cpp
    src/gnss_struct/gnss_bands.cpp
    src/gnss_struct/range_obs.cpp
    src/gnss_struct/sat_vis.cpp
    src/gnss_struct/best_pos.cpp
)

# ============================================================
# bin_io层：大文件mmap分块读取、大小端转换、字节流读取
# ============================================================
set(BIN_IO_SOURCES
    src/bin_io/bin_reader.cpp
    src/bin_io/endian_utils.cpp
)

# ============================================================
# parser层：BIN帧识别、数据块拆分、二进制转结构化数据
# ============================================================
set(PARSER_SOURCES
    src/parser/bin_parser.cpp
)

# ============================================================
# export层：解析结果导出CSV文本
# ============================================================
set(EXPORT_SOURCES
    src/export/csv_exporter.cpp
)

# ============================================================
# 所有业务模块源文件（主程序和样本生成器共用）
# ============================================================
set(COMMON_SOURCES
    ${GNSS_STRUCT_SOURCES}
    ${BIN_IO_SOURCES}
    ${PARSER_SOURCES}
    ${EXPORT_SOURCES}
)

# ============================================================
# 可执行文件1: gnss_parser（BIN解析工具）
# ============================================================
add_executable(gnss_parser
    src/main.cpp
    ${COMMON_SOURCES}
)

# ============================================================
# 可执行文件2: gnss_gen_sample（样本文件生成器）
# ============================================================
add_executable(gnss_gen_sample
    src/generate_sample.cpp
    ${GNSS_STRUCT_SOURCES}
    src/bin_io/endian_utils.cpp
)

# 链接系统库（Linux需要pthread支持mmap，Windows无需额外库）
if(UNIX AND NOT APPLE)
    target_link_libraries(gnss_parser PRIVATE pthread)
endif()

# 安装规则
install(TARGETS gnss_parser gnss_gen_sample RUNTIME DESTINATION bin)
