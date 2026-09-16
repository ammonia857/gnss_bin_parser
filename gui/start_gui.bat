@echo off
rem 启动 GNSS 解析工具的本地网页界面：只依赖 Python 标准库，无第三方包。
chcp 65001 >nul
setlocal
cd /d "%~dp0.."

where python >nul 2>nul
if errorlevel 1 (
  echo [错误] 未找到 python，请先安装 Python 3.9+ 并勾选 "Add python.exe to PATH"。
  pause
  exit /b 1
)

if not exist "build\Release\gnss_parser.exe" (
  if not exist "build\gnss_parser.exe" (
    echo [提示] 未找到 gnss_parser.exe：界面仍会启动，但需要你在顶部手动指定引擎路径，
    echo        或先按 README 的「编译」一节生成 build\Release\gnss_parser.exe。
    echo.
  )
)

echo 正在启动界面，浏览器会自动打开；关闭本窗口即结束服务。
echo 额外参数会原样传给服务端，例如： start_gui.bat --port 8800 --no-browser
echo.
python -m gui.server %*
if errorlevel 1 pause
endlocal
