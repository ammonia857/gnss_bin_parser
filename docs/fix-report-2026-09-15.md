# GNSS BIN 解析工具缺陷修复报告（2026-09-15）

> 依据：NovAtel《OEM7 Commands and Logs Reference Manual v22》+ 官方 [32-bit CRC 页](https://docs.novatel.com/OEM7/Content/Messages/32_Bit_CRC.htm) + 官方 BESTPOSB 二进制示例帧
> 验证数据：真实接收机采集 `Data_1Hz.bin`（414,767,720 字节，431,979 帧）

## 一、结论

帧的"骨架"（同步头、28 字节帧头、CRC-32、小端字节序、各日志字段偏移）原本就是正确的；
问题集中在**语义层**：星座判定读错位域、两套官方枚举混用、以及一处 OEM7 已废弃日志的错误实现。
本次修复后：**解码正确性有官方手册与官方示例帧背书，行为回退有回归测试保护**。

## 二、权威依据（关键事实）

| 事实 | 出处 |
|---|---|
| `ch-tr-status` bit16-18 = 卫星系统（0=GPS,1=GLO,2=SBAS,3=GAL,4=BDS,5=QZSS,6=NavIC,7=Other）；bit21-25 = 信号类型（含义随系统变化） | 手册 Table 156（p.806-807） |
| 日志字段 "Satellite System" 用**另一套编号**：0=GPS,1=GLONASS,2=SBAS,**5=Galileo,6=BeiDou,7=QZSS,9=NavIC** | 手册 Table 124（p.654-655），SATVIS2/HEADINGSATS 均引用此表 |
| OEM7 **没有 SATVIS(48)**，只有 SATVIS2(1043)；SATVIS 为 OEM6 旧日志 | 手册全文检索无 SATVIS 独立条目；OEM6→OEM7 变更表 "SATVIS → Replaced. Use SATVIS2" |
| BIN 为**小端**；CRC-32 = poly 0xEDB88320、初值 0、无最终异或、**小端存放**，覆盖 header+body | 官方 32-bit CRC 页及示例帧 |
| RANGE 每条观测 44 字节；BESTPOS body = 72 字节（含 H+70/H+71 两个信号掩码字节） | 手册 RANGE 表（p.804-806）、BESTPOS 表（p.522-523） |
| `glofreq` = (GLONASS Frequency + 7)；BESTPOS `hgt` 为 **MSL 高程** | 手册 RANGE 表、BESTPOS 表 |

## 三、修复清单

### Critical

| # | 问题 | 位置 | 修法 |
|---|---|---|---|
| C1 | RANGE 星座用 `bit21-25`（信号类型）反推，规则 `≤14→GPS / 17..30→BDS`，其余不赋值（未初始化读） | `src/parser/bin_parser.cpp` | 改读 `bit16-18`，经 `system_from_ch_tr_status()` 映射；`sat_system` 加默认值 |
| C2 | SATVIS(48) 使用自造布局 `4+10n`（真值 `12+40n` 且 OEM7 已删除）→ 真实数据 100% 静默丢弃 | `bin_parser.cpp`、`frame_header.h`、帮助文本 | 移除该分支，计入 `unsupported_frames` 并提示一次；帮助/README 标注为 OEM6 旧日志 |
| C3 | 内部枚举（GPS0/BDS1/GLO2/GAL3/SBAS4/QZSS5）与官方两套编号都不一致，SATVIS2 裸 `static_cast` → GLONASS 显示"北斗"、北斗显示"SBAS" | `gnss_time.h/cpp`、`bin_parser.cpp` | 新增 `system_from_ch_tr_status()` 与 `system_from_log_enum()` 两个显式映射；内部枚举补 `NAVIC/OTHER` |

### Important

| # | 问题 | 修法 |
|---|---|---|
| I1 | 长度不自洽时静默丢弃（掩盖 C2） | 新增 `ParseStats::malformed_frames / unsupported_frames` 并打印 |
| I2 | 无同步头时 `pending` 无界增长 + 二次方拷贝（`tail_len<32` 是死条件） | 找不到同步头只保留末尾 2 字节；实测 64MB 垃圾文件由 65s 降到 67ms（线性） |
| I3 | 未对齐 `reinterpret_cast` 读 double/float（ARM 上 UB/崩溃） | 全部改为 `memcpy` 版 `load_*_le()` |
| I4 | 硬要求 `hdr_len==28`（官方明说 header length 可变） | 放宽为 `>=28`，以实际 `hdr_len` 定位 body，CRC 覆盖 `hdr_len+body_len` |
| I5 | 帮助文本/注释称"大端" | 全部更正为小端 |
| I6 | `MSG_TYPE_BINARY=0` 语义错（官方：bit5-6=格式） | 改为 `MSG_TYPE_FORMAT_MASK/…_BINARY` 掩码判断 |
| I7 | 双天线 RANGE/RANGE_1 同 ID 混写 | 记录 `msg_type`（测量源）字段（尚未导出为列，见"未完成"） |
| I8 | `BinStreamReader` 跨块读取不拼接、`skip()` 少跳、`can_read()` 失效（死代码） | 整体删除，仅保留正确的 `MmapFile` + parser 的 pending 拼接 |
| I9 | `MmapFile` 构造异常泄漏句柄；`chunk_size=0` 死循环；非映射粒度报错；中文路径打不开；`reset()` 泄漏映射 | 异常路径统一清理；chunk 按系统粒度归一化；路径显式转码；`reset()` 正确解映射 |
| I10 | BESTPOS CSV 表头 `SVs_Used,SVs_Tracked` 与数据顺序相反 | 表头改为 `SVs_Tracked,SVs_Used`（写入顺序不变） |
| I11 | SATVIS2 `Health` 被浮点格式打成 `88.000000` | 改为整数输出 |
| I12 | 无论是否有数据都创建 4 个 CSV（失败留下空表头文件） | 惰性创建：首次写入才建文件 |
| I13 | 磁盘写入失败静默 | 统计并提示 `CSV 写入错误: N` |
| I14 | 定位类型表（Table 87）错值：20/48/51 等 | 按手册重建 0..80 全表（51=RTK_DIRECT_INS、69=PPP 等） |
| I15 | 样本生成器按错误位域/枚举造数据 → 测试"自证正确" | 生成器改为按官方位域（含 GPS L2C sig=17、BDS/GAL/GLO/SBAS/QZSS）、SATVIS2 按 Table124、BESTPOS 按 OEM7 尾部字段 |

### 其它一并修正

- `range_obs.h`：Message ID 100 → 43；补 `glofreq` 官方语义
- `best_pos.h`：高度语义 MSL（hae = hgt + undulation）、OEM7 尾部字段清单
- `sat_vis.h`：`sat_system` 默认值
- `gnss_time.h`：周内秒示例（345600000 实为周四）、删除"1024 周自动翻转"说法
- 新增行尾列（不破坏既有列序）：RANGE `ChTrStatus,Sat_System_Name`；SATVIS2 `GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name`

## 四、验证

### 1. 官方示例帧（权威基准）

官方手册 BESTPOSB 示例（100 字节 + 官方 CRC `42 dc 4c 48`）：

| 项 | 期望 | 实测 |
|---|---|---|
| CRC 校验 | 通过 | `crc_error = 0` |
| GPS Week / TOW | 1427 / 314158000 | 1427 / 314158000 |
| 解状态 / 定位类型 | 0 (SOL_COMPUTED) / 16 (SINGLE) | 一致 |
| 大地水准面差距 | -16.2708 | -16.2708 |
| 卫星数（tracked/used） | 11 / 11 | 11 / 11 |

### 2. 回归测试 `tests/regression.py`（6 组用例，约 60 条断言，全部 PASS）

1. 官方 BESTPOSB 示例帧逐字段
2. RANGE 同帧多系统 + **GPS L2C(sig=17) 反例**（断言必须判为 GPS 而非北斗）
3. SATVIS2 按官方 Table124（0/1/2/5/6/7/9）→ 内部枚举与名称一致；Health 为整数；负仰角保留
4. SATVIS(48) → 计入 `unsupported`，不产生 CSV
5. 坏 CRC 帧 → 被拒且数据不进 CSV
6. 脚本内用官方算法复算官方 CRC 自检

运行：`python tests/regression.py`

### 3. 真实数据前后对比（`Data_1Hz.bin`，415MB）

同一份数据，RANGE 总行数完全一致 4,313,496：

| 星座 | 修复前 | 修复后 |
|---|---|---|
| GPS | 3,705,160 | **1,557,832** |
| 北斗 | 608,336 | **2,755,664** |

- 修复前被判为"北斗"的 **608,336 行，其信号类型为 17（GPS L2C(M)）**，实为 GPS —— 修复后全部正确归为 GPS
- 修复后 `ch-tr-status` 系统位与输出星座**0 处不一致**；信号类型分布符合各系统定义（GPS: 0/9/17；北斗: 0/1/2/4/5/6）
- SATVIS2（5,010,937 行）：GPS 2,678,245 / 北斗 2,332,692（Table124 的 `6` → 北斗，映射正确）
- BESTPOS：`SVs_Tracked=26 > SVs_Used=23`，语义自洽
- 帧统计：431,979 帧（RANGE 86,396 / SATVIS2 259,187 / BESTPOS 86,396），**sync_lost=0、crc_error=0、malformed=0、unsupported=0**
- 全量耗时 103.7 秒；`satvis.csv` 不再生成（无 SATVIS 帧）

## 五、下游兼容性变化

1. BESTPOS 表头：`SVs_Used,SVs_Tracked` → **`SVs_Tracked,SVs_Used`**
2. 行尾新增列（旧列顺序不变）：RANGE `ChTrStatus,Sat_System_Name`；SATVIS2 `GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name`
3. `Sat_System` 数值含义（内部枚举 GPS=0/BDS=1/GLO=2/GAL=3/SBAS=4/QZSS=5/NAVIC=6/OTHER=7）不变，但**取值现在正确**——依赖旧错误分布的统计会明显变化
4. 定位类型码按手册修正：`51` 由 PPP → **RTK_DIRECT_INS**，`69` = PPP
5. `Height_m` 是 MSL 高程（椭球高 = `Height_m + Undulation_m`）
6. **不再支持 SATVIS(48)**（OEM6 旧日志）
7. 解析失败/无数据时不再产生只有表头的 CSV

## 六、未完成（记录待办）

- GPS 周+秒 → UTC/公历时间列；导出 `time_status`（时间质量）
- RANGE_1 / SATVIS2_1 双天线数据按测量源分流或加列
- 导出 BESTPOS 的 `ext sol stat` 与两个信号掩码、RANGE 的更多 ch-tr-status 位解释（相位锁定/奇偶已知/码锁定）
- CSV 覆盖写 vs 追加开关（当前重复运行会静默覆盖）
- 32 位目标的 >4GB 偏移支持（`_FILE_OFFSET_BITS=64` / `mmap64`）
- Linux/macOS 分支的运行时验证（本次仅有静态分析结论）
- 手册 Table 87 未定义 75/76，按 UNKNOWN 处理
