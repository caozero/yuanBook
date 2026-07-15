@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
    echo [YuanBook] CMake was not found. Install CMake and a C++17 compiler, then run this script again.
    exit /b 1
)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %ERRORLEVEL%

cmake --build build --config Release
exit /b %ERRORLEVEL%
