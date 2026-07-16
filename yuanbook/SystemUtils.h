#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SystemUtils
{
    /**
     * @brief 按当前平台 shell 规则对单个命令参数做安全引用。
     *
     * 该函数面向当前项目中的两类场景：
     * 1. 通过 `system()` 调用外部命令，例如解压 XLSX。
     * 2. 通过平台进程接口调用命令并捕获输出，例如调用 curl。
     *
     * Windows 下会生成兼容 `cmd.exe` 的双引号参数；
     * Unix-like 平台下会生成单引号参数，并正确转义内部单引号。
     *
     * @param Value 原始参数文本，可以包含空格或特殊字符。
     * @return 已按平台规则包裹和转义的命令参数字符串。
     */
    std::string ShellQuote(const std::string& Value);

    /**
     * @brief 生成位于系统临时目录中的唯一文件路径。
     *
     * 常用于写入临时请求体、临时压缩包或调试快照文件。
     * 函数只生成路径，不会主动创建文件。Windows 及 Cygwin 构建通过 Win32 API
     * 返回原生 DOS 路径，确保 Cygwin 主程序创建的文件可被 `curl.exe` 等原生进程读取。
     *
     * @param Prefix 文件名前缀，用于标识用途，例如 `cp_voice_request`。
     * @param Extension 文件扩展名，建议包含前导点，例如 `.json`。
     * @return 系统临时目录下的候选唯一路径字符串；Windows 下不会返回 `/tmp` 路径。
     * @sideeffect 不创建目录或文件；Win32 临时目录查询失败时回退到程序目录或当前目录。
     */
    std::string MakeTempFilePath(const std::string& Prefix,
                                 const std::string& Extension);

    /**
     * @brief 将 UTF-8 文本完整写入指定文件。
     *
     * 若文件不存在会创建；若已存在会覆盖。
     * 适合写入 JSON 请求体、XML 快照、日志片段等文本内容。
     *
     * @param Path 目标文件路径。
     * @param Content 要写入的完整文本。
     * @return true 表示写入成功；false 表示无法打开文件或写入失败。
     */
    bool WriteTextFile(const std::filesystem::path& Path,
                       const std::string& Content);

    /**
     * @brief 将原始二进制数据完整写入指定文件。
     *
     * 适用于上传账单、压缩包、中间缓存文件等非文本内容。
     *
     * @param Path 目标文件路径。
     * @param Content 二进制数据缓冲区。
     * @return true 表示写入成功；false 表示无法打开文件或写入失败。
     */
    bool WriteBinaryFile(const std::filesystem::path& Path,
                         const std::string& Content);

    /**
     * @brief 以二进制方式读取整个文件内容到字符串。
     *
     * 即便文件本身是文本，也按原始字节读取，避免行结束符被平台层改写。
     *
     * @param Path 要读取的文件路径。
     * @param OutContent 成功时写入完整文件内容；失败时内容未定义。
     * @return true 表示读取成功；false 表示文件不存在、无法打开或读取失败。
     */
    bool ReadFileToString(const std::filesystem::path& Path,
                          std::string& OutContent);

    /**
     * @brief 归一化子进程退出码，屏蔽 Windows 与 Unix-like 平台差异。
     *
     * Windows 下 `_pclose()` 直接返回子进程退出码；
     * Unix-like 平台下需要从 `wait` 状态中提取真实退出码或信号终止码。
     *
     * @param Status 平台原始进程结束状态值。
     * @return 统一后的退出码；发生异常时可能返回 `-1`。
     */
    int NormalizeProcessExitCode(int Status);

    /**
     * @brief 将外部程序名称解析为当前进程可访问的可执行文件路径。
     *
     * 已包含目录分隔符或属于绝对路径的输入会原样返回。Windows 下对 `curl` 这类命令名
     * 使用系统可执行文件搜索规则解析 `.exe`，并在 PATH 搜索失败时额外检查系统目录，
     * 从而降低服务进程、IDE 子进程与交互式终端 PATH 不一致造成的启动失败概率。
     * Unix-like 平台当前保持原始命令名，由 shell 按既有规则解析。
     *
     * @param ExecutablePath 配置中的可执行文件路径或命令名。
     * @return 可解析时返回稳定路径；无法解析时返回原始输入，供后续启动逻辑输出明确错误。
     * @sideeffect 不修改进程环境变量，不创建文件，也不启动任何外部进程。
     */
    std::string ResolveExecutablePath(const std::string& ExecutablePath);

    /**
     * @brief 执行命令并捕获标准输出全文。
     *
     * 该函数用于当前项目中调用 `curl` 等系统工具，并同步读取输出。
     * 它不会抛异常，而是通过返回值与输出参数表达状态。Windows 及 Cygwin 构建使用
     * Win32 `CreateProcess` 与匿名管道，不依赖 `/bin/sh` 或 `COMSPEC`；Unix-like 平台
     * 继续使用 `popen()`，以维持树莓派部署行为。
     *
     * @param Command 已完成拼接的完整命令字符串；调用方不得将鉴权密钥写入诊断日志。
     * @param OutText 成功启动进程后写入捕获到的标准输出内容。
     * @param OutCode 写入归一化后的进程退出码；若无法启动进程则保持为 `-1`。
     * @param OutStartError 可选错误输出；无法创建管道或进程时写入不含 Command 内容的诊断信息。
     * @return true 表示命令已成功启动并完成输出读取；false 表示无法创建管道进程。
     * @sideeffect 同步启动外部命令并阻塞读取其输出，直至子进程退出。
     */
    bool ReadPipeAll(const std::string& Command,
                     std::string& OutText,
                     int& OutCode,
                     std::string* OutStartError = nullptr);

    /**
     * @brief 生成仅供前端展示的敏感数据脱敏文本。
     *
     * 脱敏规则按字节长度执行：空值保持为空；长度不超过 4 的值全部替换为 `*`；
     * 其余值保留前 2 个与后 2 个字节，中间内容全部替换为 `*`。该函数只用于展示边界，
     * 禁止将返回值写回持久化存储或用于业务鉴权。
     *
     * @param Value 需要脱敏的原始敏感数据。
     * @return 不包含完整原文的展示文本；返回长度与输入字节长度一致。
     * @sideeffect 无副作用，不修改输入值。
     */
    std::string MaskSensitiveValue(const std::string& Value);

    /**
     * @brief 去除字符串首尾 ASCII 空白字符。
     *
     * 主要用于解析 XLSX / XML 文本、接口字段以及配置输入。
     *
     * @param Value 输入字符串。
     * @return 去除首尾空白后的新字符串。
     */
    std::string Trim(const std::string& Value);

    /**
     * @brief 将字符串中的 ASCII 字母原地转换为小写并返回副本。
     *
     * 只处理 ASCII 范围字母，不尝试做完整 Unicode 大小写转换。
     * 适合扩展名、HTTP 头字段、枚举文本等轻量场景。
     *
     * @param Value 输入字符串。
     * @return 小写化后的字符串副本。
     */
    std::string ToLowerAscii(std::string Value);

    /**
     * @brief 解码常见 XML entity。
     *
     * 当前只处理项目中 XLSX XML 解析所需的几类实体：
     * `&`、`<`、`>`、`"`、`'`。
     *
     * @param Value 可能包含 XML entity 的文本。
     * @return 解码后的普通文本。
     */
    std::string DecodeXmlEntities(const std::string& Value);

    /**
     * @brief 将运行时配置中的相对路径解析为稳定的绝对路径。
     *
     * 该函数专门用于本项目的可执行程序运行场景，解决以下问题：
     * 1. Debug/Release 的工作目录可能位于构建目录而不是项目根目录。
     * 2. 数据库中的系统设置可能保存为 `./www`、`./ledger.db` 这类相对路径。
     * 3. 服务启动后再次从数据库热加载路径配置时，仍需定位到真实资源目录。
     *
     * 解析顺序：
     * - 如果 `PathValue` 已经是绝对路径，则直接返回。
     * - 先尝试相对于当前工作目录解析。
     * - 再从当前可执行文件所在目录逐级向上搜索项目根锚点。
     * - 最后可选地回退到 `FallbackBaseDir` 作为基准目录。
     *
     * @param PathValue 运行时配置中的原始路径，可以是绝对路径或相对路径。
     * @param FallbackBaseDir 可选的回退基准目录；为空时跳过该策略。
     * @return 可直接用于文件系统访问的绝对路径；若无法解析则返回原始值。
     */
    std::string ResolveRuntimePath(const std::string& PathValue,
                                   const std::string& FallbackBaseDir = std::string());
}
