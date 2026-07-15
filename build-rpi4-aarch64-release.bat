@echo off
chcp 65001 >nul
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "APP_NAME=YuanBook"
set "ARCH=aarch64"
set "CONFIGURATION=Release"
set "PRESET=rpi4-aarch64-release"
set "BUILD_DIR=%ROOT%\build-rpi4-aarch64-release"
set "DIST_DIR=%ROOT%\dist\rpi4-aarch64-release"

rem 可在运行前通过环境变量覆盖以下值，例如：
rem   set RPI_TOOLCHAIN_ROOT=C:\Program Files (x86)\Arm GNU Toolchain aarch64-none-linux-gnu\14.3 rel1\bin
rem   set RPI_SYSROOT=C:\rpi\sysroot
rem   set RPI_TOOLCHAIN_PREFIX=aarch64-none-linux-gnu
if not defined RPI_TOOLCHAIN_PREFIX set "RPI_TOOLCHAIN_PREFIX=aarch64-none-linux-gnu"

if /I "%~1"=="/h" goto :help
if /I "%~1"=="/help" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="-h" goto :help

echo ========================================
echo   YuanBook Raspberry Pi 4 aarch64 Release
echo ========================================
echo Root:              %ROOT%
echo Build dir:         %BUILD_DIR%
echo Dist dir:          %DIST_DIR%
echo Toolchain prefix:  %RPI_TOOLCHAIN_PREFIX%
if defined RPI_TOOLCHAIN_ROOT echo Toolchain root:    %RPI_TOOLCHAIN_ROOT%
if defined RPI_SYSROOT echo Sysroot:           %RPI_SYSROOT%
echo.

if not exist "%ROOT%\build-rpi.ps1" (
    echo [ERROR] 未找到构建脚本: %ROOT%\build-rpi.ps1
    exit /b 1
)

if not exist "%ROOT%\scripts\package-release.ps1" (
    echo [ERROR] 未找到公共发布打包脚本: %ROOT%\scripts\package-release.ps1
    exit /b 1
)

if not exist "%ROOT%\cmake\toolchain-rpi-aarch64.cmake" (
    echo [ERROR] 未找到工具链文件: %ROOT%\cmake\toolchain-rpi-aarch64.cmake
    exit /b 1
)

if defined RPI_TOOLCHAIN_ROOT (
    if not exist "%RPI_TOOLCHAIN_ROOT%" (
        echo [ERROR] RPI_TOOLCHAIN_ROOT 不存在: %RPI_TOOLCHAIN_ROOT%
        echo [ERROR] 请设置为交叉编译工具链 bin 目录或根目录，或将 %RPI_TOOLCHAIN_PREFIX%-gcc 加入 PATH。
        exit /b 1
    )
)

if defined RPI_SYSROOT (
    if not exist "%RPI_SYSROOT%" (
        echo [ERROR] RPI_SYSROOT 不存在: %RPI_SYSROOT%
        echo [ERROR] 请设置为从树莓派同步的目标 sysroot 目录，或清空该变量。
        exit /b 1
    )
)

where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] 未找到 powershell.exe，无法调用 build-rpi.ps1。
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] 未找到 cmake，请先安装 CMake 并加入 PATH。
    exit /b 1
)

call :check_compiler "%RPI_TOOLCHAIN_PREFIX%-gcc"
if errorlevel 1 exit /b 1
call :check_compiler "%RPI_TOOLCHAIN_PREFIX%-g++"
if errorlevel 1 exit /b 1

pushd "%ROOT%" >nul
if errorlevel 1 (
    echo [ERROR] 无法进入项目根目录: %ROOT%
    exit /b 1
)

echo [1/3] Build main target: powershell.exe -ExecutionPolicy Bypass -File build-rpi.ps1 -Arch %ARCH% -Configuration %CONFIGURATION% -BuildDir "%BUILD_DIR%" -Clean
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\build-rpi.ps1" -Arch %ARCH% -Configuration %CONFIGURATION% -BuildDir "%BUILD_DIR%" -Clean
if errorlevel 1 (
    set "BUILD_CODE=%ERRORLEVEL%"
    popd >nul
    echo [ERROR] 树莓派 aarch64 Release 编译失败，exit code=%BUILD_CODE%。
    exit /b %BUILD_CODE%
)

echo.
echo [2/3] Verify target architecture and run cross-build smoke checks
call :find_built_executable
if errorlevel 1 (
    popd >nul
    exit /b 1
)
call :verify_aarch64_executable "%BUILT_EXE%"
if errorlevel 1 (
    popd >nul
    exit /b 1
)

echo.
echo [3/3] Package runtime files: %DIST_DIR%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\package-release.ps1" -ProjectRoot "%ROOT%" -ExecutablePath "%BUILT_EXE%" -DistDir "%DIST_DIR%" -OutputExecutableName "%APP_NAME%"
set "PACKAGE_CODE=%ERRORLEVEL%"
popd >nul
if not "%PACKAGE_CODE%"=="0" (
    echo [ERROR] 树莓派 aarch64 Release 公共文件归集失败，exit code=%PACKAGE_CODE%。
    exit /b %PACKAGE_CODE%
)

echo.
echo [OK] YuanBook 树莓派 4 aarch64 Release 已编译并归集完成。
echo [OK] 发布目录: %DIST_DIR%
echo [OK] 已包含: %APP_NAME%、www、content、ledger.db^(如存在^)；不包含 ledger.db-wal/ledger.db-shm。
exit /b 0

:help
echo YuanBook Raspberry Pi 4 aarch64 Release 一键交叉编译脚本
echo.
echo 用法:
echo   build-rpi4-aarch64-release.bat
echo   build-rpi4-aarch64-release.bat --help
echo.
echo 可选环境变量:
echo   RPI_TOOLCHAIN_PREFIX  GNU 工具链前缀，默认 aarch64-none-linux-gnu
echo   RPI_TOOLCHAIN_ROOT    工具链 bin 目录或根目录
echo   RPI_SYSROOT           目标树莓派 sysroot 目录
echo.
echo 示例:
echo   set RPI_TOOLCHAIN_ROOT=C:\Program Files (x86)\Arm GNU Toolchain aarch64-none-linux-gnu\14.3 rel1\bin
echo   set RPI_SYSROOT=C:\rpi\sysroot
echo   set RPI_TOOLCHAIN_PREFIX=aarch64-none-linux-gnu
echo   build-rpi4-aarch64-release.bat
echo.
echo 输出:
echo   build-rpi4-aarch64-release\       CMake 构建目录
echo   dist\rpi4-aarch64-release\        运行发布目录
echo.
echo 说明:
echo   脚本会先清理旧构建目录并仅构建 YuanBook 主程序。
echo   公共打包脚本会复制 www、content 和可选 ledger.db；不会复制 ledger.db-wal/ledger.db-shm。
exit /b 0

:check_compiler
set "COMPILER_NAME=%~1"
set "FOUND_COMPILER="

if defined RPI_TOOLCHAIN_ROOT (
    if exist "%RPI_TOOLCHAIN_ROOT%\%COMPILER_NAME%" set "FOUND_COMPILER=%RPI_TOOLCHAIN_ROOT%\%COMPILER_NAME%"
    if exist "%RPI_TOOLCHAIN_ROOT%\%COMPILER_NAME%.exe" set "FOUND_COMPILER=%RPI_TOOLCHAIN_ROOT%\%COMPILER_NAME%.exe"
    if exist "%RPI_TOOLCHAIN_ROOT%\bin\%COMPILER_NAME%" set "FOUND_COMPILER=%RPI_TOOLCHAIN_ROOT%\bin\%COMPILER_NAME%"
    if exist "%RPI_TOOLCHAIN_ROOT%\bin\%COMPILER_NAME%.exe" set "FOUND_COMPILER=%RPI_TOOLCHAIN_ROOT%\bin\%COMPILER_NAME%.exe"
)

if defined FOUND_COMPILER exit /b 0
where %COMPILER_NAME% >nul 2>nul
if not errorlevel 1 exit /b 0

echo [ERROR] 未找到交叉编译器: %COMPILER_NAME%
echo [ERROR] 请安装 Raspberry Pi 4 aarch64 GNU 交叉工具链，并将其加入 PATH；
echo [ERROR] 或设置 RPI_TOOLCHAIN_ROOT 为工具链 bin 目录/根目录；如工具链前缀不同，请设置 RPI_TOOLCHAIN_PREFIX。
echo [ERROR] 当前 RPI_TOOLCHAIN_PREFIX=%RPI_TOOLCHAIN_PREFIX%
exit /b 1

:find_built_executable
set "BUILT_EXE="
if exist "%BUILD_DIR%\%APP_NAME%" set "BUILT_EXE=%BUILD_DIR%\%APP_NAME%"
if exist "%BUILD_DIR%\%APP_NAME%.exe" set "BUILT_EXE=%BUILD_DIR%\%APP_NAME%.exe"
if exist "%BUILD_DIR%\Release\%APP_NAME%" set "BUILT_EXE=%BUILD_DIR%\Release\%APP_NAME%"
if exist "%BUILD_DIR%\Release\%APP_NAME%.exe" set "BUILT_EXE=%BUILD_DIR%\Release\%APP_NAME%.exe"

if not defined BUILT_EXE (
    echo [ERROR] 未找到编译产物: %BUILD_DIR%\%APP_NAME%
    echo [ERROR] 请检查 CMake 输出目录。
    exit /b 1
)
exit /b 0

:verify_aarch64_executable
set "VERIFY_EXE=%~1"
set "READELF_COMMAND=%RPI_TOOLCHAIN_PREFIX%-readelf"
set "READELF_PATH="

if defined RPI_TOOLCHAIN_ROOT (
    if exist "%RPI_TOOLCHAIN_ROOT%\%READELF_COMMAND%.exe" set "READELF_PATH=%RPI_TOOLCHAIN_ROOT%\%READELF_COMMAND%.exe"
    if exist "%RPI_TOOLCHAIN_ROOT%\%READELF_COMMAND%" set "READELF_PATH=%RPI_TOOLCHAIN_ROOT%\%READELF_COMMAND%"
    if exist "%RPI_TOOLCHAIN_ROOT%\bin\%READELF_COMMAND%.exe" set "READELF_PATH=%RPI_TOOLCHAIN_ROOT%\bin\%READELF_COMMAND%.exe"
    if exist "%RPI_TOOLCHAIN_ROOT%\bin\%READELF_COMMAND%" set "READELF_PATH=%RPI_TOOLCHAIN_ROOT%\bin\%READELF_COMMAND%"
)

if not defined READELF_PATH (
    for /f "delims=" %%P in ('where %READELF_COMMAND% 2^>nul') do if not defined READELF_PATH set "READELF_PATH=%%~fP"
)

if not defined READELF_PATH (
    echo [ERROR] 未找到目标架构检查工具: %READELF_COMMAND%
    echo [ERROR] 无法确认发布产物是否为 AArch64 ELF，请检查交叉工具链完整性。
    exit /b 1
)

"%READELF_PATH%" -h "%VERIFY_EXE%" 2>nul | findstr /I /C:"Machine:" | findstr /I /C:"AArch64" >nul
if errorlevel 1 (
    echo [ERROR] 编译产物不是预期的 AArch64 ELF: %VERIFY_EXE%
    exit /b 1
)

echo [OK] 已确认编译产物架构为 AArch64 ELF。
exit /b 0
