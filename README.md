# gnss_bin_parser

NovAtel OEM7 GNSS BIN 解析工具 —— 纯 C++17 实现的跨平台命令行工具，用于解析 NovAtel OEM7 接收机输出的 BIN 格式数据文件，并导出为结构化 CSV。

## 功能

- 解析 NovAtel OEM7 BIN 格式（同步头 `0xAA 0x44 0x12`，28 字节 OEM7 帧头，官方允许更长的帧头，解析按 `hdr_len` 定位消息体）
- 支持以下消息帧：

| MsgID | 日志 | 说明 | 支持 |
| --- | --- | --- | --- |
| `42` | BESTPOS | 定位结果（纬度/经度/**MSL 高程**/精度/卫星数） | ✅ |
| `43` | RANGE | 观测数据（伪距/载波相位/多普勒/CN0/ch-tr-status） | ✅ |
| `1043` | SATVIS2 | 卫星可见性扩展（仰角/方位角/健康度/GLONASS 频率/多普勒） | ✅ |
| `48` | SATVIS | 卫星可见性 | ❌ **不支持，详见下文** |

- 大文件 mmap 分块读取，高性能
- 支持大小端字节序转换，跨平台（Windows / Linux / macOS）
- 单文件解析、指定输出目录、批量解析整个文件夹
- CSV **惰性建文件**：某类数据没有时不会创建对应文件；全程无数据则不创建任何 CSV
- CSV 写入失败可见（写入/刷盘/关闭失败会累加错误计数，并在统计中打印警告）
- 附带样本文件生成器 `gnss_gen_sample`，按官方位域/枚举生成测试用 BIN 文件
- 附带防回归测试 `tests/regression.py`

### 为什么不支持 SATVIS(48)

`SATVIS`（MsgID 48）是 **OEM6 时代的旧日志**，OEM7 固件已不再输出该日志（OEM7 手册中只有 `SATVIS2`，MsgID 1043），且其二进制布局与本工具早期的假设并不一致。因此本工具**不再解析** SATVIS(48)：遇到时只计入 `ParseStats::unsupported_frames`（并在解析日志里提示一次），既不误报为畸形帧，也不静默丢弃。样本生成器也不再产出该帧。

## 字节序与 CRC

- **NovAtel BIN 的所有多字节字段（帧头、消息体、帧尾 CRC32）都是小端（little-endian）存放。**
  本条早期文档曾误写为"大端"，已更正。
- **CRC-32**：多项式 `0xEDB88320`（反射的 IEEE 802.3），初值 `0`，输入/输出均不取反（**无最终异或**），
  校验范围 = 帧头 + 消息体（不含 CRC 本身），计算结果**小端存放**在帧尾 4 字节。
  官方文档的 C 参考实现即 `CalculateBlockCRC32()`。

## 编译

依赖 CMake ≥ 3.16 与支持 C++17 的编译器（MSVC / GCC / Clang）。

```bash
cmake -B build
cmake --build build --config Release
```

Windows（VS 自带 cmake）示例：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'build' --config Release
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

输出文件（**惰性创建**：只有实际解析到该类数据才会创建并写表头）：

| 文件 | 内容 |
| --- | --- |
| `{prefix}_range.csv` | RANGE 观测数据 |
| `{prefix}_satvis2.csv` | SATVIS2 卫星可见性扩展 |
| `{prefix}_bestpos.csv` | BESTPOS 定位结果 |
| `{prefix}_satvis.csv` | SATVIS 可见性（**OEM7 无此日志，实际不会产生**；导出器仍保留该接口供外部调用） |

## CSV 列定义

追加列一律位于**行尾**，既有列顺序保持不变，避免破坏下游脚本。

| 文件 | 列 |
| --- | --- |
| RANGE | `GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,PsrStd_m,CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,CN0_dBHz,LockTime_s` **+** `ChTrStatus,Sat_System_Name` |
| SATVIS2 | `GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg,Health` **+** `GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name` |
| SATVIS | `GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg` **+** `Sat_System_Name` |
| BESTPOS | `GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,Longitude_deg,Height_m,Undulation_m,LatStd_m,LonStd_m,HgtStd_m,SVs_Tracked,SVs_Used` |

新增/修正列说明：

- **`ChTrStatus`**（RANGE）：`ch-tr-status` 原始 32 位状态字，十六进制输出（如 `0x02200000`），便于核对星座/信号位域。
- **`GloFreq`**（RANGE / SATVIS2）：语义为 **GLONASS Frequency + 7**（NovAtel 官方定义），非 GLONASS 为 `0`。
  例如 GLONASS 频率通道 `1` → `GloFreq = 8`。
- **`Health`**（SATVIS2）：以**无符号整数**输出（不再受定点格式影响而带小数点）。
- **`TrueDoppler_Hz` / `ApparentDoppler_Hz`**（SATVIS2）：理论/视多普勒（Hz）。
- **`Sat_System_Name`**（RANGE / SATVIS / SATVIS2）：系统名（`GPS`/`北斗`/`GLONASS`/`Galileo`/`SBAS`/`QZSS`/`NavIC`/`其它`/`未知`），便于人读。
- **BESTPOS 表头修正**：早期表头写作 `...,SVs_Used,SVs_Tracked`，但写入顺序是 `#SVs(tracked)` 在前、
  `#solnSVs(used)` 在后，二者语义颠倒；现表头改为 `...,SVs_Tracked,SVs_Used`（写入顺序不变）。
  其中 `Height_m` 是 **MSL 高程（海拔）**，椭球高 = `Height_m + Undulation_m`。

## 三套卫星系统编号对照

本工具内部**不使用**任何一套官方编号，解析时统一映射为内部枚举；CSV 的 `Sat_System` 列打印的就是内部枚举。

**内部枚举（CSV `Sat_System` 列）**

| 值 | 系统 |
| --- | --- |
| 0 | GPS |
| 1 | BDS（北斗） |
| 2 | GLONASS |
| 3 | GALILEO |
| 4 | SBAS |
| 5 | QZSS |
| 6 | NAVIC |
| 7 | OTHER |
| 255 | UNKNOWN |

**RANGE `ch-tr-status` 的星座字段（bit16-18，官方 Table 156）**

| 值 | 系统 |
| --- | --- |
| 0 | GPS |
| 1 | GLONASS |
| 2 | SBAS |
| 3 | Galileo |
| 4 | BeiDou |
| 5 | QZSS |
| 6 | NavIC |
| 7 | Other |

> 注意：`ch-tr-status` 的 **bit21-25 是信号类型**（含义随系统变化，例如 GPS `0`=L1C/A、`17`=L2C(M)、BDS `0`=B1I、`9`=B2a、Galileo `12`=E5a），
> **不能用它反推星座**。早期实现正是按 bit21-25 反推，导致 GPS L2C（sys=0,sig=17）被误判成北斗。

**日志字段 `Satellite System`（SATVIS2 等日志，官方 Table 124）**

| 值 | 系统 |
| --- | --- |
| 0 | GPS |
| 1 | GLONASS |
| 2 | SBAS |
| 5 | Galileo |
| 6 | BeiDou |
| 7 | QZSS |
| 9 | NavIC |

> 这套编号与 `ch-tr-status` 的星座编号**不是同一套**（例如 Galileo 在 Table 156 中是 3，在 Table 124 中是 5），
> 因此**禁止**把原始字段直接 `static_cast` 成内部枚举，必须经由 `system_from_ch_tr_status()` /
> `system_from_log_enum()` 显式映射（定义于 `include/gnss_struct/gnss_time.h`）。
> 官方未占用的值（如 3/4/8）一律映射为 `UNKNOWN`。

## 回归测试

`tests/regression.py` 用 Python 标准库按**官方算法**（poly `0xEDB88320`、init `0`、无最终异或、小端存放）
自行构造 BIN 帧，调用 `build/Release/gnss_parser.exe` 解析并校验 CSV，覆盖：

1. 官方手册 BESTPOSB 示例帧（官方字节 + 官方 CRC `42 dc 4c 48`）→ 校验 lat/lon/undulation/卫星数；
2. RANGE 同帧多系统（`sys=0/1/3/4`）与 **GPS L2C(M)（sys=0, sig=17）必须判为 GPS 而非北斗**；
3. SATVIS2 系统字段按官方 Table 124（`0/1/2/5/6/7/9`）→ 内部枚举 `0,2,4,3,1,5,6`，并校验 `Sat_System_Name`；
4. SATVIS(48) 不得被解析且计入 `unsupported_frames`；
5. CRC 被改坏的帧计入 `crc_error`，且其数据不出现在 CSV 中；
6. CSV 惰性建文件（无数据不产生文件）。

运行：

```bash
python tests/regression.py                    # 自动推导 build/Release/gnss_parser.exe
python tests/regression.py --exe <路径>       # 指定被测可执行文件
python tests/regression.py --keep             # 保留临时输出目录以便排查
```

全部通过时退出码为 `0`；任一断言失败打印期望/实际 diff 并以非 0 退出。

## 代码结构

```
include/            # 头文件
  bin_io/           #   mmap 分块读取、小端读取辅助（load_*_le）、字节流读取
  gnss_struct/      #   GNSS 数据结构体（时间、观测、可见性、定位）+ 系统枚举与映射
  parser/           #   BIN 帧识别、数据块拆分
  export/           #   CSV 导出
src/                # 实现文件
  main.cpp          #   gnss_parser 命令行入口
  generate_sample.cpp # gnss_gen_sample 样本生成器（按官方位域/枚举生成）
tests/
  regression.py     #   防回归测试
```
