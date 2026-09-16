# GUI P1 交付与验证报告

日期：2026-09-16 ｜ 分支：`feat/gui-p1` ｜ 计划：[`plans/2026-09-15-gnss-gui-p1.md`](superpowers/plans/2026-09-15-gnss-gui-p1.md) ｜ 设计：[`specs/2026-09-15-gnss-gui-design.md`](superpowers/specs/2026-09-15-gnss-gui-design.md)

## 1. 结论

P1 的 10 项任务全部完成：**双击 `gui\start_gui.bat` 即可用**的本地网页界面（Python 标准库 + 零构建前端），
选文件/拖拽入队 → 实时进度 → 结束后看帧数、星座分布、CSV 清单 → 一键打开输出目录。

验证口径（全部为本人实际运行结果，非推断）：

| 项目 | 命令 | 结果 |
| --- | --- | --- |
| 界面测试套件 | `python -m unittest discover -s tests/gui -t .` | **55 项全通过**（含 10 项真实引擎端到端） |
| 引擎回归 | `python tests/regression.py` | **PASS**（6 组断言，官方字节/CRC） |
| 真实数据端到端 | `python tests\gui\perf_smoke.py C:\Users\...\Data_1Hz.bin` | **done**，395.6 MB → 431,979 帧 / RANGE 4,313,496 行 |
| 手工界面冒烟 | 起服务后按前端路径驱动 HTTP（settings→jobs→SSE→summary→rows→filter） | **PASS**，SSE 事件序列 `hello → job → job → progress → job` |

## 2. 交付物

| 路径 | 说明 |
| --- | --- |
| `gui/static/index.html` `app.js` `style.css` | 中文单页界面：工具条、任务队列、详情（统计卡片/星座条/输出文件）、整页拖拽上传、SSE 实时刷新 |
| `gui/server.py` | HTTP 路由、SSE 广播、上传落盘、结果聚合出口；`create_server(..., state_path=)` 便于测试隔离 |
| `gui/jobs.py` | 串行任务队列（单线程 + 单子进程）、引擎探测、stdout 进度/统计解析、前缀判重、状态机与持久化 |
| `gui/aggregate.py` | 输出 CSV 流式统计 + 行号索引分页（不整文件读入内存） |
| `gui/native_picker.py` | tkinter 原生对话框桥（对话框在主线程，HTTP 线程排队等待） |
| `gui/state.py` | `gui/state.json` 原子持久化、损坏容错、设置白名单 |
| `gui/start_gui.bat` `gui/README.md` | 双击启动脚本与界面文档（主 README 增加「图形界面」一节） |
| `tests/gui/*` | 假引擎、状态/解析/队列/聚合/对话框/HTTP/前端资源/端到端测试，外加手动性能脚本 |

分工上，界面只做**包装与呈现**：真正的解析仍由 `gnss_parser.exe` 完成，因此引擎侧的正确性由
`tests/regression.py` 单独把关，界面侧只需证明「经界面得到的结果与直接跑 exe 完全一致」——见 §4。

## 3. 测试清单（55 项）

| 测试文件 | 数量 | 覆盖 |
| --- | --- | --- |
| `test_smoke.py` | 1 | 假引擎可被队列正确驱动 |
| `test_state.py` | 5 | 默认值、白名单、损坏文件回退、原子写、字段契约 |
| `test_jobs_parse.py` | 4 | 进度行/统计行解析、前缀清洗与判重 |
| `test_jobs_queue.py` | 9 | 串行执行与统计、失败退出码、运行中取消、前缀防覆盖、中断恢复、预检；**autoStart 关闭不得漏跑**、**排队态前缀判重** |
| `test_aggregate.py` | 5 | 星座分布、时间跨度、索引分页、过滤、缓存键 |
| `test_picker.py` | 4 | 对话框请求/响应管线（注入假实现，不弹真窗口） |
| `test_server.py` | 7 | 设置往返、任务生命周期、上传、409 分支、404/400 |
| `test_frontend_assets.py` | 10 | 三个静态文件存在与引用、`node --check` 语法、接口路径齐全、无外链、DOM id 与 JS 对齐、静态资源 Content-Type、`/api/pick/outdir` 与 `/api/jobs/start` |
| `test_e2e.py` | 10 | 真实引擎：统计逐项相等、实测命名规则、CSV 的 BOM+CRLF、列数与 README 一致、星座分布、分页与 `filterSystem`、缓存、**与直接跑 exe 逐字节一致**、空文件/垃圾文件失败原因、同名输入换前缀 + 手动放行 |

## 4. 真实引擎端到端（tests/gui/test_e2e.py）

夹具（`tests/gui/fixtures.py`）复用 `tests/regression.py` 的官方 CRC/位域构帧代码，一个文件里塞进
3 个 RANGE 历元（GPS/GLONASS/BDS/Galileo 各 1 颗，共 12 行）、2 个 SATVIS2 帧（GPS 2 颗 + BDS 3 颗）、
1 个**官方 BESTPOSB 示例帧**、1 个 SATVIS(48) 旧日志帧、1 个 CRC 被改坏的帧、以及尾部噪声。

关键断言与实测值：

- `totalFrames = 6`（3 RANGE + 2 SATVIS2 + 1 BESTPOS；不支持帧与坏 CRC 帧不计入）；
- `rangeRows = 12`、`satvis2Rows = 5`、`bestposRows = 1`、`crcErrors = 1`、`unsupported = 1`；
- 输出文件名 = `gnss_demo_range.csv` / `gnss_demo_satvis2.csv` / `gnss_demo_bestpos.csv`，
  即**实测规则 `{前缀}_{输入主名}_{类型}.csv`**（`-p gnss` 时引擎打印 `输出前缀: gnss_demo`）；
- CSV 以 `EF BB BF` 开头、全部 CRLF、列数分别 14 / 11 / 13 与 README 一致；
- 星座分布 `{"GPS": 3, "GLONASS": 3, "北斗": 3, "Galileo": 3}`，SATVIS2 为 `{"GPS": 2, "北斗": 3}`；
- **G-U-I 与直接跑 exe 的三个 CSV 逐字节相同**（`assertEqual` 比较 `read_bytes()`）。

## 5. 真实大数据性能（395.6 MB，431,979 帧）

`python tests\gui\perf_smoke.py C:\Users\31743\Downloads\Data_1Hz.bin`（三次运行结果一致）：

| 指标 | 实测 |
| --- | --- |
| 解析总耗时 | 60.3 / 60.8 / 62.0 s（引擎自报 60.2 / 60.6 / 61.8 s） |
| 吞吐 | 6.4 ~ 6.6 MB/s |
| 解析结果 | 431,979 帧；RANGE 4,313,496 行、SATVIS2 5,010,937 行、BESTPOS 86,396 行；CRC 错误 0 |
| 输出体积 | `range.csv` 422.5 MB + `satvis2.csv` 281.4 MB + `bestpos.csv` 7.5 MB |
| 概览统计（首扫 422.5 MB / 431 万行） | 8.69 ~ 8.83 s（≈48 MB/s），**命中缓存 ≈0 s** |
| 行号索引 | 863 个检查点，1.11 ~ 1.12 s |
| 分页读取 100 行 | 首页 / 中部 / 尾部均 **< 10 ms**（不随页码变慢） |
| 界面进程内存峰值 | **26.0 MB**（计划要求 < 500 MB） |
| 星座分布（真实数据） | GPS 1,557,832 行 / 北斗 2,755,664 行 |

真实数据的星座分布与修复后引擎的既有结论一致（旧 bug 曾给出 GPS 3,705,160 / "BDS" 608,336），
说明界面链路没有引入任何偏移。统计与分页都是**流式**实现：界面进程内存峰值仅 **26 MB**，
与 422 MB 的 CSV 体积无关。

## 6. 本轮发现并修复的问题

| # | 问题 | 影响 | 处理 |
| --- | --- | --- | --- |
| 1 | 队列的放行开关在**进入阻塞取任务之前**判定，判定通过后 worker 已阻塞在待办队列上 | 关掉「拖入即开始」后紧接着入队，任务照样执行（开关形同虚设） | 改为在同一把锁里「判定放行 + 按列表顺序取任务」，并删除独立的待办 id 队列（顺带消灭删除/清空后残留 id 的隐患）；`test_jobs_queue` + `test_e2e` 双重回归 |
| 2 | `unique_prefix` 只查文件系统，不看队列 | 同名输入连续入队两次 → 同一前缀 → 第二个任务的 CSV 覆盖第一个 | 把队列里其它任务**将要产出**的文件名纳入判重；有单元与端到端回归 |
| 3 | 夹具误用 `OFFICIAL_BESTPOSB`（它只是 28+72 字节的帧头+body，不含 4 字节 CRC） | 端到端测试里官方 BESTPOS 帧被判 `malformed`，期望行数对不上 | 拼上 `OFFICIAL_BESTPOSB_CRC`；这正是"先写期望再跑真实引擎"的价值 |
| 4 | 主 README 把输出文件写成 `{prefix}_range.csv` | 与实测的 `{前缀}_{主名}_{类型}.csv` 不符，用户按文档找不到文件 | 修正 README（并说明 `-p` 默认 `gnss`） |
| 5 | 界面缺少「选择输出目录」的后端接口，`autoStart` 关闭时也没有放行入口 | 设计稿要求的能力无法从前端触达 | 新增 `POST /api/pick/outdir` 与 `POST /api/jobs/start`（含测试），前端按钮相应接线 |
| 6 | 测试直接用仓库根目录起服务 | 会污染用户的 `gui/state.json`（任务历史串场，测试互相干扰） | `create_server(..., state_path=)` 支持独立状态文件，测试全部改用临时状态 |

## 7. 已知边界（P2/P3 待做）

- 队列**串行**（一次一个子进程）：几百 MB 数据不并行抢磁盘，代价是多文件时总耗时线性叠加。
- 拖拽上传会把文件复制到 `%TEMP%\gnss_gui_uploads\`，大文件请用「选择 BIN 文件…」（原生对话框，零拷贝）；
  `cleanupUploads` 设置项已预留但尚未实现自动清理。
- 尚无图表（进度曲线/星座饼图）、无报告导出与一键打包 zip、无按星座过滤的原始行表格 UI
  （`/api/results/{id}/rows` 已支持 `filterSystem` 与分页，P2/P3 可直接接）。
- 只监听 `127.0.0.1`，无鉴权：本工具面向单机使用，不要改成对外监听。
