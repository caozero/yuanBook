# YuanBook C++ 构建说明

## 依赖

- CMake 3.16 或更新版本
- 支持 C++17 的编译器
  - Windows: Visual Studio Build Tools / MSVC，或 MinGW-w64
  - Linux / 树莓派: g++ 或 clang++
- cpp-httplib 单头文件 `httplib.h`
  - 已可直接放置到 `yuanbook/httplib.h`，CMake 默认会搜索该目录
  - 下载地址: https://github.com/yhirose/cpp-httplib
  - 或在 CMake 配置时传入 `-DYUANBOOK_THIRD_PARTY_INCLUDE_DIR=<包含 httplib.h 的目录>`
- SQLite3 开发文件
  - 项目支持直接使用 `yuanbook/sqlite3.h` 与 `yuanbook/sqlite3.c` 的 SQLite amalgamation
  - Windows 默认启用本地 `yuanbook/sqlite3.c` 源码构建，通常不需要额外安装 SQLite
  - Linux / 树莓派默认优先使用系统 SQLite（Debian / Raspberry Pi OS: `sudo apt install libsqlite3-dev`），找不到系统库时会自动回退到 `yuanbook/sqlite3.c`

## 构建与发布矩阵

| 目标 | 推荐入口 | 默认 SQLite 来源 | 发布目录 |
|---|---|---|---|
| Windows 本机开发构建 | `build.ps1` 或 `build.bat` | 仓库内置 `yuanbook/sqlite3.c` | `build/` |
| Windows Release 发布 | `build-windows-release.bat` | 仓库内置源码，除非显式覆盖 | `dist/windows-release/` |
| Linux / 树莓派本机构建 | 直接使用 CMake | 优先系统 SQLite，缺失时回退内置源码 | CMake 构建目录 |
| Windows → 树莓派 aarch64 | `build-rpi.ps1 -Arch aarch64` | 脚本强制使用内置源码 | 指定构建目录 |
| 树莓派 4 aarch64 Release | `build-rpi4-aarch64-release.bat` | 内置源码 | `dist/rpi4-aarch64-release/` |
| Windows → 树莓派 armhf | `build-rpi.ps1 -Arch armhf` | 脚本强制使用内置源码 | 指定构建目录 |

发布归集脚本会复制可执行文件、`www` 和 `content`，并按规则选择性复制数据库模板。构建目录中的程序适合开发验证；面向部署时优先使用 `dist` 下归集完整的发布目录。

## Windows 构建

```powershell
./build.ps1
```

默认会使用 `yuanbook/httplib.h`，并在 Windows 上编译本地 `yuanbook/sqlite3.c`。如需手动指定第三方头文件或已有 SQLite 库，可配置：

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYUANBOOK_THIRD_PARTY_INCLUDE_DIR=C:/path/to/include -DYUANBOOK_SQLITE3_LIBRARY=C:/path/to/sqlite3.lib
cmake --build build --config Release
```

如需强制使用系统/外部 SQLite 而不是本地源码，可传入：

```powershell
cmake -S . -B build -DYUANBOOK_USE_BUNDLED_SQLITE=OFF
```

## Linux / 树莓派本机构建

```bash
sudo apt update
sudo apt install -y cmake g++ libsqlite3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/YuanBook -http=5080
```

如果目标设备没有安装 `libsqlite3-dev`，但仓库内存在 `yuanbook/sqlite3.c`，CMake 会自动回退为本地源码构建；也可以显式传入：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYUANBOOK_USE_BUNDLED_SQLITE=ON
```

## Windows 到树莓派 Linux 交叉编译

仓库提供两个 CMake toolchain 文件：

- `cmake/toolchain-rpi-aarch64.cmake`：树莓派 64 位 Linux，GNU 前缀默认 `aarch64-none-linux-gnu`（与外部 `cpHomeCenter` 项目使用的 Arm GNU Toolchain 一致），推荐默认目标。
- `cmake/toolchain-rpi-armhf.cmake`：树莓派 32 位 Linux，GNU 前缀默认 `arm-linux-gnueabihf`。

交叉编译不联网下载依赖；请先通过 MSYS2、预装 GNU 交叉工具链或其他方式让对应的 `*-gcc` / `*-g++` 可被 Windows CMake 调用。可选提供从目标系统同步来的 sysroot，以便查找目标平台头文件和库。

推荐使用 PowerShell 脚本：

```powershell
./build-rpi.ps1 -Arch aarch64
```

常用参数：

```powershell
# 指定交叉工具链 bin 目录或根目录
./build-rpi.ps1 -Arch aarch64 -ToolchainRoot "C:/Program Files (x86)/Arm GNU Toolchain aarch64-none-linux-gnu/14.3 rel1/bin"

# 指定目标 sysroot
./build-rpi.ps1 -Arch aarch64 -Sysroot C:/rpi/sysroot

# 32 位 Raspberry Pi OS 目标
./build-rpi.ps1 -Arch armhf

# 只执行 CMake 配置检查，不编译
./build-rpi.ps1 -Arch aarch64 -ConfigureOnly
```

也可以直接调用 CMake：

```powershell
cmake -S . -B build-rpi-aarch64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rpi-aarch64.cmake -DRPI_TOOLCHAIN_ROOT="C:/Program Files (x86)/Arm GNU Toolchain aarch64-none-linux-gnu/14.3 rel1/bin" -DRPI_SYSROOT=C:/rpi/sysroot -DYUANBOOK_USE_BUNDLED_SQLITE=ON
cmake --build build-rpi-aarch64 --config Release
```

可通过环境变量减少命令行参数：

```powershell
$env:RPI_TOOLCHAIN_ROOT = 'C:/Program Files (x86)/Arm GNU Toolchain aarch64-none-linux-gnu/14.3 rel1/bin'
$env:RPI_SYSROOT = 'C:/rpi/sysroot'
$env:RPI_TOOLCHAIN_PREFIX = 'aarch64-none-linux-gnu'
./build-rpi.ps1 -Arch aarch64
```

脚本默认传入 `-DYUANBOOK_USE_BUNDLED_SQLITE=ON`，避免交叉环境缺少目标平台 `libsqlite3-dev` 时误链接到宿主机 SQLite；如已准备好目标 sysroot 和目标 SQLite，也可直接用 CMake 改为 `-DYUANBOOK_USE_BUNDLED_SQLITE=OFF`。

### Windows 一键批处理：树莓派 4 / aarch64 / Release

仓库提供可双击运行的批处理：

```cmd
build-rpi4-aarch64-release.bat
```

该批处理会调用 `build-rpi.ps1`，固定目标为 Raspberry Pi 4 / aarch64 / Release；构建成功后使用交叉工具链的 `readelf` 强制确认产物为 AArch64 ELF，最后通过公共 `scripts/package-release.ps1` 刷新发布目录：

```text
build-rpi4-aarch64-release\
dist\rpi4-aarch64-release\
```

发布目录包含运行所需的 `YuanBook` 可执行文件、`www` 静态资源目录、`content` 内容目录，以及存在时从 `yuanbook/ledger.db`（其次为项目根目录 `ledger.db`）复制的数据库模板。公共脚本使用临时目录完成归集，全部复制成功后才替换当前项目内的 `dist/rpi4-aarch64-release` 发布目录，不会删除外部路径。

SQLite 的 `ledger.db-wal` 与 `ledger.db-shm` 属于运行时伴生文件，不会进入发布目录；日志、备份和测试数据库也不会被归集。若需要把当前业务数据库作为正式模板发布，应先停止正在运行的 YuanBook 服务，确保主数据库已完成检查点并处于一致状态。

可编辑/覆盖的环境变量：

```cmd
set RPI_TOOLCHAIN_ROOT=C:\Program Files (x86)\Arm GNU Toolchain aarch64-none-linux-gnu\14.3 rel1\bin
set RPI_SYSROOT=C:\rpi\sysroot
set RPI_TOOLCHAIN_PREFIX=aarch64-none-linux-gnu
build-rpi4-aarch64-release.bat
```

如果工具链使用 `aarch64-none-linux-gnu` 等不同前缀，请相应设置 `RPI_TOOLCHAIN_PREFIX`。查看帮助和前置检查说明：

```cmd
build-rpi4-aarch64-release.bat --help
```

## Windows Release 一键构建与发布

Windows Release 批处理：

```cmd
build-windows-release.bat
```

该脚本会先清理旧构建目录，只构建 `YuanBook.exe` 主目标，然后调用公共 `scripts/package-release.ps1`，将程序、`www`、`content` 和可选 `ledger.db` 原子归集到：

```text
dist\windows-release\
```

Windows 专属的运行时 DLL 收集仍由 `build-windows-release.bat` 在公共资源归集成功后执行。Windows 与树莓派发布脚本均从干净构建目录开始，并通过 `--target YuanBook` 限定为主程序目标，不配置、不编译、不运行测试程序。两平台发布包采用相同的数据库规则，均不复制 `ledger.db-wal`、`ledger.db-shm`、日志、备份和测试数据库。

## 发布包运行要求

完整发布目录至少应保留：

```text
YuanBook 或 YuanBook.exe
www/
content/
```

`www` 缺失时浏览器页面无法加载；`content` 用于随包部署内容资源。`ledger.db` 为可选数据库模板：不携带时，程序会在目标数据库路径初始化新库；携带时应确保模板来自已停止服务且完成 SQLite 检查点的一致状态。

运行产生的数据库、`ledger.db-wal`、`ledger.db-shm`、日志和 `bak` 备份目录属于实例数据，不应在升级时被发布包无条件覆盖。

## 最小化构建策略

项目 CMake 配置仅定义 `YuanBook` 主程序，不生成 CTest 元数据，也不定义任何测试可执行目标。Windows 与树莓派 Release 脚本进一步显式指定 `--target YuanBook`，确保发布检查只编译运行所需内容。

仓库中的 `tests/` 目录仅保留历史测试源码，不参与默认构建或 Release 构建；如需重新启用测试，应在独立开发分支中显式恢复对应 CMake 目标，避免污染发布产物。

## 常见构建问题

- 找不到 `httplib.h`：确认仓库存在 `yuanbook/httplib.h`，或传入 `YUANBOOK_THIRD_PARTY_INCLUDE_DIR`。
- 找不到 SQLite：安装目标平台开发包，传入 `YUANBOOK_SQLITE3_LIBRARY`，或启用 `YUANBOOK_USE_BUNDLED_SQLITE=ON`。
- 交叉编译误用宿主机库：优先使用 `build-rpi.ps1` 的默认内置 SQLite 策略；仅在 sysroot 完整时切换到目标系统 SQLite。
- 发布后页面 404：确认可执行文件旁保留完整 `www` 目录，或启动时使用 `-www=PATH` 指向正确位置。

