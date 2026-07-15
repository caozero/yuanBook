@echo off
chcp 65001 >nul
setlocal EnableExtensions DisableDelayedExpansion

rem ============================================================
rem YuanBook GitHub 提交脚本
rem
rem 功能：
rem   1. 检查 Git、仓库及远程仓库配置。
rem   2. 将当前工作区全部变更加入暂存区。
rem   3. 创建本地提交，并推送当前分支至 origin。
rem
rem 用法：
rem   submit-github.bat "提交说明"
rem   submit-github.bat
rem
rem 注意：脚本会执行 git add -A，包括新增、修改和删除的文件。
rem ============================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "REMOTE_NAME=origin"
set "COMMIT_MESSAGE=%~1"

if /I "%~1"=="/h" goto :help
if /I "%~1"=="/help" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help

echo ========================================
echo   YuanBook GitHub 提交工具
echo ========================================
echo 项目目录: %ROOT%
echo.

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] 未找到 Git，请先安装 Git 并将其加入 PATH。
    exit /b 1
)

pushd "%ROOT%" >nul
if errorlevel 1 (
    echo [ERROR] 无法进入项目目录: %ROOT%
    exit /b 1
)

git rev-parse --is-inside-work-tree >nul 2>nul
if errorlevel 1 (
    popd >nul
    echo [ERROR] 当前目录不是 Git 仓库: %ROOT%
    exit /b 1
)

git remote get-url "%REMOTE_NAME%" >nul 2>nul
if errorlevel 1 (
    popd >nul
    echo [ERROR] 未配置远程仓库 origin。
    echo [ERROR] 请先执行: git remote add origin GitHub仓库地址
    exit /b 1
)

for /f "delims=" %%B in ('git branch --show-current 2^>nul') do set "CURRENT_BRANCH=%%B"
if not defined CURRENT_BRANCH (
    popd >nul
    echo [ERROR] 当前处于 detached HEAD 状态，无法确定需要推送的分支。
    exit /b 1
)

for /f "delims=" %%R in ('git remote get-url "%REMOTE_NAME%" 2^>nul') do set "REMOTE_URL=%%R"

echo 当前分支: %CURRENT_BRANCH%
echo 远程仓库: %REMOTE_URL%
echo.
echo 当前工作区状态:
git status --short
echo.

if not defined COMMIT_MESSAGE (
    set /p "COMMIT_MESSAGE=请输入提交说明: "
)

if not defined COMMIT_MESSAGE (
    popd >nul
    echo [ERROR] 提交说明不能为空。
    exit /b 1
)

echo [1/3] 正在暂存全部变更...
git add -A
if errorlevel 1 (
    popd >nul
    echo [ERROR] 暂存文件失败。
    exit /b 1
)

git diff --cached --quiet
if errorlevel 1 (
    echo.
    echo [2/3] 正在创建提交...
    git commit -m "%COMMIT_MESSAGE%"
    if errorlevel 1 (
        popd >nul
        echo [ERROR] 创建 Git 提交失败。
        exit /b 1
    )
) else (
    echo.
    echo [2/3] 暂存区没有新变更，跳过创建提交。
)

echo.
echo [3/3] 正在推送分支 %CURRENT_BRANCH% 至 %REMOTE_NAME%...
git push -u "%REMOTE_NAME%" "%CURRENT_BRANCH%"
set "PUSH_CODE=%ERRORLEVEL%"
popd >nul

if not "%PUSH_CODE%"=="0" (
    echo [ERROR] 推送 GitHub 失败，exit code=%PUSH_CODE%。
    echo [ERROR] 若远程分支包含本地没有的提交，请先拉取并处理冲突后重试。
    exit /b %PUSH_CODE%
)

echo.
echo [OK] 当前分支已成功提交并推送到 GitHub。
exit /b 0

:help
echo YuanBook GitHub 提交工具
echo.
echo 用法:
echo   submit-github.bat "提交说明"
echo   submit-github.bat
echo.
echo 示例:
echo   submit-github.bat "修正 README 换行问题"
echo.
echo 未通过参数提供提交说明时，脚本会提示手工输入。
echo 脚本会暂存全部新增、修改和删除的文件，并推送当前分支至 origin。
exit /b 0
