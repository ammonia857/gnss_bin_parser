# gnss_bin_parser

NovAtel OEM7 GNSS BIN 解析工具 —— 纯 C++17 实现的跨平台命令行工具，用于解析 NovAtel OEM7 接收机输出的 BIN 格式数据文件，并导出为结构化 CSV。

## 功能

- 解析 NovAtel OEM7 BIN 格式（同步头 `0xAA 0x44 0x12`，28 字节 OEM7 帧头）
- 支持以下消息帧：
  - `MsgID 42` BESTPOS —— 定位结果（纬度/经度/大地高/精度）
  - `MsgID 43` RANGE —— 观测数据（伪距/载波相位/多普勒/CN0）
  - `MsgID 48` SATVIS —— 卫星可见性（仰角/方位角）
  - `MsgID 55` SATVIS2 —— 卫星可见性扩展（仰角/方位角/健康度）
- 大文件 mmap 分块读取，高性能
- 支持大小端字节序转换，跨平台（Windows / Linux / macOS）
- 单文件解析、指定输出目录、批量解析整个文件夹
- 附带样本文件生成器 `gnss_gen_sample`，可生成测试用 BIN 文件

## 编译

依赖 CMake ≥ 3.16 与支持 C++17 的编译器（MSVC / GCC / Clang）。

```bash
cmake -B build
cmake --build build --config Release
```

生成两个可执行文件：`gnss_parser`（解析工具）与 `gnss_gen_sample`（样本生成器）。

## 用法

```
gnss_parser -i data.bin                          # 单文件解析，输出到 ./output
gnss_parser -i data.bin -o ./result              # 指定输出目录
gnss_parser -d ./bin_folder -o ./result          # 批量解析文件夹内所有 .bin
gnss_parser -i data.bin -p myprefix              # 自定义输出文件前缀
gnss_parser -v                                   # 详细输出模式
gnss_parser --help                               # 显示帮助
```

输出文件：

| 文件 | 内容 |
| --- | --- |
| `{prefix}_range.csv` | RANGE 观测数据 |
| `{prefix}_satvis.csv` | SATVIS 卫星可见性 |
| `{prefix}_satvis2.csv` | SATVIS2 卫星可见性扩展 |
| `{prefix}_bestpos.csv` | BESTPOS 定位结果 |

## 代码结构

```
include/            # 头文件
  bin_io/           #   mmap 分块读取、大小端转换、字节流读取
  gnss_struct/      #   GNSS 数据结构体（时间、观测、可见性、定位）
  parser/           #   BIN 帧识别、数据块拆分
  export/           #   CSV 导出
src/                # 实现文件
  main.cpp          #   gnss_parser 命令行入口
  generate_sample.cpp # gnss_gen_sample 样本生成器
```
