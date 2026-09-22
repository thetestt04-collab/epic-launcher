@echo off
setlocal EnableExtensions EnableDelayedExpansion

chcp 65001 >nul

for /f %%a in ('copy /Z "%~f0" nul') do set "CR=%%a"

set "TOOLCHAIN_BIN="
for %%D in ("C:\msys64\mingw64\bin" "%USERPROFILE%\w64devkit\bin" "C:\w64devkit\bin" "D:\msys64\mingw64\bin") do (
    if not defined TOOLCHAIN_BIN if exist "%%~D\g++.exe" set "TOOLCHAIN_BIN=%%~D"
)
if defined TOOLCHAIN_BIN set "PATH=%TOOLCHAIN_BIN%;%PATH%"

set "IUP_DIR=external\iup-3.30_Win64_dllw6_lib"
set "WINDIVERT_DIR=external\WinDivert-2.2.2-A"
set "OBJ_DIR=build\obj"
set "OUTPUT_DIR=build"

where g++ >nul 2>&1 || (
    echo [ERROR] g++ was not found.
    echo Install a MinGW-w64 toolchain ^(MSYS2 or w64devkit^) and add it to
    echo PATH, or extract w64devkit to "%USERPROFILE%\w64devkit".
    pause
    exit /b 1
)
where windres >nul 2>&1 || (
    echo [ERROR] windres was not found. Install the MinGW-w64 toolchain.
    pause
    exit /b 1
)
if not exist "%IUP_DIR%\include\iup.h" goto missing_dependencies
if not exist "%IUP_DIR%\libiup.a" goto missing_dependencies
if not exist "%WINDIVERT_DIR%\include\windivert.h" goto missing_dependencies
if not exist "%WINDIVERT_DIR%\x64\WinDivert.lib" goto missing_dependencies

set "LTO_FLAG="
echo int lto_probe(void){return 0;}> "%TEMP%\lto_probe.c"
>nul 2>&1 g++ -flto -c "%TEMP%\lto_probe.c" -o "%TEMP%\lto_probe.o"
if not errorlevel 1 set "LTO_FLAG=-flto"
del "%TEMP%\lto_probe.c" "%TEMP%\lto_probe.o" >nul 2>&1

if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if exist "%OUTPUT_DIR%\config.txt" del /Q "%OUTPUT_DIR%\config.txt"
if exist "%OUTPUT_DIR%\EpicGamesLauncher.exe" del /Q "%OUTPUT_DIR%\EpicGamesLauncher.exe"

echo [1/3] Compiling resources...
windres etc\epic-games.rc -O coff -o "%OBJ_DIR%\epic_games_res.o" -D X64
if errorlevel 1 goto build_failed

echo [2/3] Compiling C++17 sources...
set "OBJECTS="
set "FILE_COUNT=0"
for %%F in (src\*.cpp) do set /a FILE_COUNT+=1
if %FILE_COUNT% EQU 0 goto build_failed
set "DONE_COUNT=0"
call :showProgress
for %%F in (src\*.cpp) do (
    set "OBJECT=%OBJ_DIR%\%%~nF.o"
    g++ -c "%%F" -o "!OBJECT!" ^
      -I "%IUP_DIR%\include" -I src -I "%WINDIVERT_DIR%\include" ^
      -std=c++17 -O2 %LTO_FLAG% -DNDEBUG -Wall -Wextra -Wpedantic ^
      -Wconversion -Wshadow -Wformat=2 -Wno-cast-function-type -m64
    if errorlevel 1 goto build_failed
    set "OBJECTS=!OBJECTS! "!OBJECT!""
    set /a DONE_COUNT+=1
    call :showProgress
)
echo.

echo [3/3] Linking optimized executable...
g++ !OBJECTS! "%OBJ_DIR%\epic_games_res.o" -o "%OUTPUT_DIR%\EpicGamesLauncher.exe" ^
  -L "%IUP_DIR%" -L "%WINDIVERT_DIR%\x64" ^
  -liup -lWinDivert -lcomctl32 -lwinmm -lws2_32 -liphlpapi -lwinhttp -lshell32 ^
  -lkernel32 -lgdi32 -lcomdlg32 -luuid -lole32 ^
  %LTO_FLAG% -O2 -s -mwindows -m64 -static
if errorlevel 1 goto build_failed

fc /B "%WINDIVERT_DIR%\x64\WinDivert.dll" "%OUTPUT_DIR%\WinDivert.dll" >nul 2>&1
if errorlevel 1 (
    copy /Y "%WINDIVERT_DIR%\x64\WinDivert.dll" "%OUTPUT_DIR%\" >nul
    if errorlevel 1 goto build_failed
)
fc /B "%WINDIVERT_DIR%\x64\WinDivert64.sys" "%OUTPUT_DIR%\WinDivert64.sys" >nul 2>&1
if errorlevel 1 (
    copy /Y "%WINDIVERT_DIR%\x64\WinDivert64.sys" "%OUTPUT_DIR%\" >nul
    if errorlevel 1 goto build_failed
)
fc /B "%IUP_DIR%\iup.dll" "%OUTPUT_DIR%\iup.dll" >nul 2>&1
if errorlevel 1 (
    copy /Y "%IUP_DIR%\iup.dll" "%OUTPUT_DIR%\" >nul
    if errorlevel 1 goto build_failed
)
fc /B config.json "%OUTPUT_DIR%\config.json" >nul 2>&1
if errorlevel 1 (
    copy /Y config.json "%OUTPUT_DIR%\" >nul
    if errorlevel 1 goto build_failed
)

if exist "%OBJ_DIR%" rmdir /S /Q "%OBJ_DIR%"

echo.
echo [SUCCESS] %OUTPUT_DIR%\EpicGamesLauncher.exe is ready.
echo Run it as Administrator.
exit /b 0

:showProgress
set /a PCT=!DONE_COUNT!*100/!FILE_COUNT!
if !PCT! GTR 100 set "PCT=100"
set "BAR="
for /l %%i in (1,1,!FILE_COUNT!) do (
    if %%i LEQ !DONE_COUNT! (set "BAR=!BAR!█") else (set "BAR=!BAR!░")
)
<nul set /p "=   Compiling [!BAR!] !PCT!%%   !CR!"
exit /b 0

:missing_dependencies
echo.
echo [ERROR] Build dependencies are missing.
echo Run: powershell -ExecutionPolicy Bypass -File .\setup-deps.ps1
pause
exit /b 1

:build_failed
echo.
echo [FAILED] Build stopped because a command failed.
pause
exit /b 1

