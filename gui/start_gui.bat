@echo off
rem ===========================================================================
rem  GNSS BIN parser - local web UI launcher
rem
rem  NOTE: keep this file ASCII-only. Non-ASCII text inside a .bat file makes
rem  cmd.exe mis-parse lines (it re-reads the file by byte offset), which shows
rem  up as "'xxx' is not recognized as an internal or external command".
rem  Chinese messages are printed by the Python side instead.
rem
rem  The project folder is located automatically from this script's location.
rem  If this file is copied elsewhere (e.g. onto the Desktop), DEFAULT_ROOT is
rem  used as a fallback - so a desktop copy still works.
rem ===========================================================================
setlocal EnableExtensions
title GNSS BIN parser - web UI

set "DEFAULT_ROOT=C:\Users\31743\Desktop\CC deepseek\gnss_bin_parser"

set "ROOT=%~dp0.."
if not exist "%ROOT%\gui\server.py" set "ROOT=%DEFAULT_ROOT%"
if not exist "%ROOT%\gui\server.py" goto :noproject

pushd "%ROOT%"
set "PYTHONPATH=%ROOT%"

rem ---- find a Python interpreter -------------------------------------------
set "PY=python"
where python >nul 2>nul
if not errorlevel 1 goto :havepython
where py >nul 2>nul
if not errorlevel 1 goto :usepy
goto :nopython
:usepy
set "PY=py"
:havepython

rem ---- warn early if the engine has not been built -------------------------
if exist "%ROOT%\build\Release\gnss_parser.exe" goto :engineok
if exist "%ROOT%\build\gnss_parser.exe" goto :engineok
echo [WARN] gnss_parser.exe not found under build\.
echo        The page still starts, but you must type the engine path at the top,
echo        or build it first (see the README "Build" section).
echo.

:engineok
echo Project folder : %ROOT%
echo Python         : %PY%
echo.
echo Starting the local web UI. The browser opens automatically.
echo Keep this window open while you use the page; close it to stop the service.
echo Extra arguments are passed through, e.g.:
echo     start_gui.bat --port 8800 --no-browser
echo.

%PY% -m gui.server %*
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" goto :failed
endlocal
exit /b 0

:failed
echo.
echo [ERROR] The server exited with code %RC%.
pause
endlocal
exit /b %RC%

:noproject
echo [ERROR] Cannot find gui\server.py.
echo         Tried next to this script and: %DEFAULT_ROOT%
echo         Fix: edit the DEFAULT_ROOT line in this file, or run it from the
echo              project folder (gui\start_gui.bat).
pause
exit /b 1

:nopython
echo [ERROR] Python 3 was not found in PATH.
echo         Install Python 3.9+ and tick "Add python.exe to PATH", then retry.
pause
exit /b 1
