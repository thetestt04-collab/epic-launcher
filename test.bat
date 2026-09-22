@echo off
setlocal EnableExtensions
set "TOOLCHAIN_BIN="
for %%D in ("C:\msys64\mingw64\bin" "%USERPROFILE%\w64devkit\bin" "C:\w64devkit\bin" "D:\msys64\mingw64\bin") do (
    if not defined TOOLCHAIN_BIN if exist "%%~D\g++.exe" set "TOOLCHAIN_BIN=%%~D"
)
if defined TOOLCHAIN_BIN set "PATH=%TOOLCHAIN_BIN%;%PATH%"

set "IUP_DIR=external\iup-3.30_Win64_dllw6_lib"
set "WINDIVERT_DIR=external\WinDivert-2.2.2-A"

if not exist build mkdir build

g++ tests\packet_core_tests.cpp src\packet.cpp src\packet_pool.cpp ^
  -I "%IUP_DIR%\include" -I src -I "%WINDIVERT_DIR%\include" ^
  -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow ^
  -Wformat=2 -Werror -m64 -o build\packet_core_tests.exe
if errorlevel 1 exit /b 1

build\packet_core_tests.exe
if errorlevel 1 exit /b 1

g++ tests\config_tests.cpp src\config.cpp src\hotkey.cpp -I src ^
  -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow ^
  -Wformat=2 -Werror -m64 -o build\config_tests.exe
if errorlevel 1 exit /b 1

build\config_tests.exe
set "TEST_RESULT=%errorlevel%"

if errorlevel 1 exit /b 1

g++ tests\update_tests.cpp src\update.cpp -I src ^
  -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow ^
  -Wformat=2 -Werror -m64 -lwinhttp -lshell32 -o build\update_tests.exe
if errorlevel 1 exit /b 1

build\update_tests.exe
set "TEST_RESULT=%errorlevel%"

if exist build\packet_core_tests.exe del /Q build\packet_core_tests.exe
if exist build\config_tests.exe del /Q build\config_tests.exe
if exist build\update_tests.exe del /Q build\update_tests.exe

exit /b %TEST_RESULT%

