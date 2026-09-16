# GUI：本地网页界面（P1）

给 `gnss_parser` 套一层**本地网页界面**，用于「塞进数据 → 看进度 → 看统计 → 打开 CSV」这条最短路径。
面向不熟悉命令行的使用场景：双击 `start_gui.bat` 就能用。

## 快速开始

```bat
:: 方式一：双击
gui\start_gui.bat

:: 方式二：命令行（在仓库根目录）
python -m gui.server
python -m gui.server --port 8800 --no-browser      # 指定端口 / 不自动开浏览器
```

启动后控制台会打印 `界面已启动: http://127.0.0.1:<端口>/`，浏览器自动打开。
**只监听 127.0.0.1**，不对外开放；端口默认 8765，被占用时自动向后试到 8780。

依赖：**Python 3.9+ 标准库**（`http.server`、`tkinter`、`json`…），无需 pip 安装任何东西。
`tkinter` 缺失时界面仍可用，只是「选择文件/目录」按钮失效，可改用拖拽或手填路径。

### 关于 `start_gui.bat`

脚本会先按**自身所在位置**推算项目目录；算不出来时退回文件里写死的 `DEFAULT_ROOT`
（本机为 `C:\Users\31743\Desktop\CC deepseek\gnss_bin_parser`）。因此把它复制到桌面、
开始菜单等任何地方双击都能用；项目整体搬家后，改 `DEFAULT_ROOT` 那一行即可。

> 注意：该文件必须保持**纯 ASCII**。`.bat` 里出现中文等多字节字符会让 cmd.exe 按字节偏移
> 重新读取时错位，表现为控制台报 `'xxx' is not recognized as an internal or external command`。
> 中文提示一律由 Python 侧打印（Python 在 Windows 控制台走 UTF-16 写屏，不受代码页影响）。

## 功能（P1）

| 区域 | 能力 |
| --- | --- |
| 顶部工具条 | 显示/修改引擎路径（留空=自动探测）、输出目录（可选）、「拖入即开始」开关、「选择 BIN 文件…」「选择文件夹…」「开始解析」「清空队列」 |
| 左侧队列 | 任务列表 + 状态徽标（排队中/解析中/完成/失败/已取消/已中断）、实时进度条、单任务「取消 / 重试 / 打开目录 / 删除」 |
| 右侧详情 | 统计卡片（总帧数、RANGE/SATVIS2/BESTPOS 行数、CRC 错误、非法帧、耗时、吞吐、退出码）、星座分布条、输出 CSV 列表（可点开）、失败原因 |
| 拖拽上传 | 把 `.bin` 拖到页面上即可入队，带上传进度；右上角提示「会先复制一份到临时目录」，大文件建议用「选择 BIN 文件…」走零拷贝的原生对话框 |
| 实时性 | SSE（`/api/events`）推送任务状态与进度；断线自动重连并重新拉取全量任务列表做校准 |
| 连接状态 | 工具条左侧有指示灯（已连接/未连接）。启动窗口被关掉后指示灯变红并给出重连提示，页面每 3 秒自动重试；窗口重新打开（或按 F5）后自动恢复，已选任务的统计会自动补上 |
| 统计失败可恢复 | 概览统计（要扫几百 MB CSV）失败时不再卡在"正在统计…"，而是显示原因和「重新统计」按钮 |
| 刷新后自动选中 | 页面刷新后自动选中最近一个任务并补齐统计，不用每次手点 |

界面重启时，上一轮仍在「解析中」的任务会被标记为**已中断**（不会假装还在跑），可一键重试。

## 目录结构

| 文件 | 职责 |
| --- | --- |
| `gui/server.py` | HTTP 服务与路由（`create_server()` 便于测试注入）、SSE 广播、上传落盘、结果聚合出口 |
| `gui/jobs.py` | 串行任务队列（单工作线程 + 单子进程）、引擎探测、stdout 进度/统计解析、输出前缀判重、任务状态机与持久化 |
| `gui/aggregate.py` | 输出 CSV 的流式统计（星座分布、时间跨度）与行号索引分页读取 |
| `gui/native_picker.py` | tkinter 原生对话框桥（对话框必须在主线程 → 请求队列 + 常驻循环） |
| `gui/state.py` | 设置与任务列表持久化到 `gui/state.json`（原子写盘） |
| `gui/static/` | 零构建前端：`index.html` / `app.js` / `style.css`（无框架、无外链） |
| `gui/start_gui.bat` | Windows 双击启动脚本 |

## HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` `/static/*` | 前端页面与静态资源 |
| GET/POST | `/api/settings` | 读取/更新设置（`enginePath`、`outputDir`、`autoStart`、`cleanupUploads`、`port`） |
| GET | `/api/events` | SSE：`hello`（全量快照+设置）、`job`（任务变化）、`progress`（进度与统计） |
| POST | `/api/pick/files` `/api/pick/folder` `/api/pick/outdir` | 弹原生对话框，返回绝对路径（前端拿不到本地路径，必须由后端弹窗） |
| POST | `/api/upload` | 拖拽兜底：请求体即文件字节，文件名走 `X-Filename` 头 |
| POST/GET | `/api/jobs` | 入队（`paths` 数组 + 可选 `uploaded`）/ 读取全量任务 |
| POST | `/api/jobs/start` `/api/jobs/clear` | 手动放行排队任务（`autoStart` 关闭时）/ 清空非运行中任务 |
| POST | `/api/jobs/{id}/cancel\|retry\|remove` | 单任务操作；状态不允许时返回 409 |
| GET | `/api/results/{id}/summary\|rows` | 星座分布与文件清单 / 分页读原始行（`dataset`、`offset`、`limit`、`filterSystem`） |
| POST | `/api/open` | 用系统默认程序打开输出目录或 CSV（**仅限本工具输出目录内**） |

## 配置与清理

- 设置和任务历史存在 `gui/state.json`（已加入 `.gitignore`），删掉即恢复默认。
- 拖拽上传的副本放在 `%TEMP%\gnss_gui_uploads\`，可手动删除；`cleanupUploads` 设置项保留给后续版本自动清理。
- 解析结果默认写到仓库下的 `parsed_output/`（已加入 `.gitignore`）。

## 测试

```bat
python -m unittest discover -s tests/gui -t .
```

覆盖：设置持久化、stdout 进度/统计解析、队列状态机（含重试/取消/删除与 409 分支）、CSV 聚合与分页、原生对话框请求管线、HTTP 路由、前端资源契约（语法、DOM id 对齐、接口路径齐全）、以及**真实引擎**端到端（`tests/gui/test_e2e.py`）。

## 已知边界

- 队列**串行**执行：一次只跑一个子进程，避免几百 MB 数据并行时抢磁盘。
- `/api/open` 只允许打开本工具输出目录内的路径；上传副本所在临时目录不在白名单内。
- 浏览器拖拽拿不到真实路径，因此大文件请优先用「选择 BIN 文件…」（原生对话框，零拷贝）。
- **端口独占**：已显式关闭 `SO_REUSEADDR`。Windows 上这个选项允许"抢占"别人正在监听的
  端口，会导致第二个实例绑到同一端口、两个服务争抢连接（表现为队列忽然变空）。现在重复启动
  会老老实实顺延端口，控制台打印的地址才是有效地址。
- 服务进程被关闭（比如误关了那个黑窗口）后，页面上的操作会失败；此时按工具条红色指示灯的提示
  重新启动即可，页面会自己恢复，**已解析好的 CSV 不受影响**。
