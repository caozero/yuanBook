@echo off
chcp 65001 >nul
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "APP_NAME=YuanBook"
set "CONFIGURATION=Release"
set "BUILD_DIR=%ROOT%\build-windows-release"
set "DIST_DIR=%ROOT%\dist\windows-release"

if /I "%~1"=="/h" goto :help
if /I "%~1"=="/help" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="-h" goto :help

echo ========================================
echo   YuanBook Windows Release
echo ========================================
echo Root:              %ROOT%
echo Build dir:         %BUILD_DIR%
echo Dist dir:          %DIST_DIR%
echo Configuration:     %CONFIGURATION%
echo.

if not exist "%ROOT%\CMakeLists.txt" (
    echo [ERROR] 未找到 CMakeLists.txt: %ROOT%\CMakeLists.txt
    exit /b 1
)

if not exist "%ROOT%\scripts\package-release.ps1" (
    echo [ERROR] 未找到公共发布打包脚本: %ROOT%\scripts\package-release.ps1
    exit /b 1
)

where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] 未找到 powershell.exe，无法执行公共发布打包脚本。
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] 未找到 cmake，请先安装 CMake 并加入 PATH。
    exit /b 1
)

pushd "%ROOT%" >nul
if errorlevel 1 (
    echo [ERROR] 无法进入项目根目录: %ROOT%
    exit /b 1
)

rem 每次从干净构建目录开始，避免旧版本遗留的测试目标或可执行文件混入发布检查。
if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
if exist "%BUILD_DIR%" (
    popd >nul
    echo [ERROR] 无法清理旧构建目录: %BUILD_DIR%
    exit /b 1
)

rem CMake 可能来自 MSYS2；在项目根目录内使用相对路径，避免其错误拼接 Windows 盘符路径。
echo [1/3] Configure: cmake -S . -B "build-windows-release" -DCMAKE_BUILD_TYPE=%CONFIGURATION% -DYUANBOOK_USE_BUNDLED_SQLITE=ON
cmake -S . -B "build-windows-release" -DCMAKE_BUILD_TYPE=%CONFIGURATION% -DYUANBOOK_USE_BUNDLED_SQLITE=ON
if errorlevel 1 (
    set "CONFIG_CODE=1"
    popd >nul
    echo [ERROR] Windows Release 配置失败。
    exit /b 1
)

echo.
echo [2/3] Build main target only: cmake --build "build-windows-release" --config %CONFIGURATION% --target YuanBook
cmake --build "build-windows-release" --config %CONFIGURATION% --target YuanBook
if errorlevel 1 (
    set "BUILD_CODE=1"
    popd >nul
    echo [ERROR] Windows Release 主程序编译失败。
    exit /b 1
)

echo.
echo [3/3] Package runtime files: %DIST_DIR%
call :find_built_executable
if errorlevel 1 (
    popd >nul
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\package-release.ps1" -ProjectRoot "%ROOT%" -ExecutablePath "%BUILT_EXE%" -DistDir "%DIST_DIR%" -OutputExecutableName "%APP_NAME%.exe"
set "PACKAGE_CODE=%ERRORLEVEL%"
if not "%PACKAGE_CODE%"=="0" (
    popd >nul
    echo [ERROR] Windows Release 公共文件归集失败，exit code=%PACKAGE_CODE%。
    exit /b %PACKAGE_CODE%
)

call :copy_runtime_dlls "%BUILT_EXE%" "%DIST_DIR%"
set "DLL_CODE=%ERRORLEVEL%"
popd >nul
if not "%DLL_CODE%"=="0" exit /b %DLL_CODE%

echo.
echo [OK] YuanBook Windows Release 已编译并归集完成。
echo [OK] 发布目录: %DIST_DIR%
echo [OK] 已包含: %APP_NAME%.exe、www、content、ledger.db^(如存在^) 以及检测到的运行时 DLL；不包含 ledger.db-wal/ledger.db-shm。
exit /b 0

:help
echo YuanBook Windows Release 一键编译发布脚本
echo.
echo 用法:
echo   build-windows-release.bat
echo   build-windows-release.bat --help
echo.
echo 前置条件:
echo   1. 安装 CMake 并加入 PATH
echo   2. 安装可用的 Windows C++17 编译器，例如 Visual Studio Build Tools、MinGW-w64 或 LLVM/Clang
echo.
echo 输出:
echo   build-windows-release\       CMake 构建目录
echo   dist\windows-release\        Windows 运行发布目录
echo.
echo 说明:
echo   脚本会先清理旧构建目录，仅构建 YuanBook 主程序，并启用项目内置 SQLite 源码。
echo   公共打包脚本会复制 Web 静态资源、content、数据库模板和可检测到的运行时 DLL。
echo   SQLite 运行时伴生文件 ledger.db-wal 与 ledger.db-shm 不会进入发布目录。
exit /b 0

:find_built_executable
set "BUILT_EXE="
if exist "%BUILD_DIR%\%APP_NAME%.exe" set "BUILT_EXE=%BUILD_DIR%\%APP_NAME%.exe"
if exist "%BUILD_DIR%\%CONFIGURATION%\%APP_NAME%.exe" set "BUILT_EXE=%BUILD_DIR%\%CONFIGURATION%\%APP_NAME%.exe"
if exist "%BUILD_DIR%\bin\%APP_NAME%.exe" set "BUILT_EXE=%BUILD_DIR%\bin\%APP_NAME%.exe"
if exist "%BUILD_DIR%\bin\%CONFIGURATION%\%APP_NAME%.exe" set "BUILT_EXE=%BUILD_DIR%\bin\%CONFIGURATION%\%APP_NAME%.exe"

if not defined BUILT_EXE (
    echo [ERROR] 未找到编译产物: %APP_NAME%.exe
    echo [ERROR] 请检查 CMake 输出目录: %BUILD_DIR%
    exit /b 1
)
exit /b 0

:copy_runtime_dlls
set "EXE_PATH=%~1"
set "TARGET_DIR=%~2"
for %%F in ("%EXE_PATH%") do set "EXE_DIR=%%~dpF"

rem 复制可执行文件旁边已经生成/部署的 DLL。
for %%D in ("%EXE_DIR%*.dll") do if exist "%%~fD" copy /Y "%%~fD" "%TARGET_DIR%\" >nul

rem 优先使用 objdump 读取可执行文件的直接 DLL 依赖，并从 PATH 中精确归集非系统运行库。
where objdump >nul 2>nul
if not errorlevel 1 (
    for /f "tokens=3" %%D in ('objdump -p "%EXE_PATH%" 2^>nul ^| findstr /C:"DLL Name:"') do call :copy_dependency_from_path "%%D" "%TARGET_DIR%"
)

rem 对 MinGW/LLVM-MinGW 构建保留已知运行库兜底归集逻辑。
for %%C in (g++ clang++ c++) do (
    for /f "delims=" %%P in ('where %%C 2^>nul') do call :copy_known_runtime_dlls "%%~dpP" "%TARGET_DIR%"
)

exit /b 0

:copy_dependency_from_path
set "DEPENDENCY_NAME=%~1"
set "TARGET_DIR=%~2"
if not defined DEPENDENCY_NAME exit /b 0
for %%S in (KERNEL32.dll USER32.dll ADVAPI32.dll SHELL32.dll WS2_32.dll ntdll.dll) do if /I "%DEPENDENCY_NAME%"=="%%S" exit /b 0
for /f "delims=" %%P in ('where "%DEPENDENCY_NAME%" 2^>nul') do (
    copy /Y "%%~fP" "%TARGET_DIR%\" >nul
    exit /b 0
)
exit /b 0

:copy_known_runtime_dlls
set "RUNTIME_DIR=%~1"
set "TARGET_DIR=%~2"
if exist "%RUNTIME_DIR%libgcc_s_seh-1.dll" copy /Y "%RUNTIME_DIR%libgcc_s_seh-1.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libgcc_s_sjlj-1.dll" copy /Y "%RUNTIME_DIR%libgcc_s_sjlj-1.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libgcc_s_dw2-1.dll" copy /Y "%RUNTIME_DIR%libgcc_s_dw2-1.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libstdc++-6.dll" copy /Y "%RUNTIME_DIR%libstdc++-6.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%msys-2.0.dll" copy /Y "%RUNTIME_DIR%msys-2.0.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%msys-gcc_s-seh-1.dll" copy /Y "%RUNTIME_DIR%msys-gcc_s-seh-1.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%msys-stdc++-6.dll" copy /Y "%RUNTIME_DIR%msys-stdc++-6.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libwinpthread-1.dll" copy /Y "%RUNTIME_DIR%libwinpthread-1.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libc++.dll" copy /Y "%RUNTIME_DIR%libc++.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libc++abi.dll" copy /Y "%RUNTIME_DIR%libc++abi.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%libunwind.dll" copy /Y "%RUNTIME_DIR%libunwind.dll" "%TARGET_DIR%\" >nul
if exist "%RUNTIME_DIR%zlib1.dll" copy /Y "%RUNTIME_DIR%zlib1.dll" "%TARGET_DIR%\" >nul
exit /b 0

