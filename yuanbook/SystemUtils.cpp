#include "SystemUtils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SystemUtils
{
    namespace
    {
        static bool IsAbsolutePathString(const std::string& PathValue)
        {
            if (PathValue.empty()) return false;
#if defined(_WIN32) || defined(__CYGWIN__)
            if (PathValue.size() >= 2 && PathValue[1] == ':') return true;
            if (PathValue[0] == '/' || PathValue[0] == '\\') return true;
#else
            if (PathValue[0] == '/') return true;
#endif
            return false;
        }

        static std::string GetExecutableDirectory()
        {
#if defined(_WIN32) || defined(__CYGWIN__)
            char PathBuffer[32768] = {};
            if (!GetModuleFileNameA(nullptr, PathBuffer, static_cast<DWORD>(sizeof(PathBuffer)))) {
                return "";
            }
            return std::filesystem::path(PathBuffer).parent_path().string();
#else
            char PathBuffer[1024];
            const ssize_t Length = readlink("/proc/self/exe", PathBuffer, sizeof(PathBuffer) - 1);
            if (Length <= 0) {
                return "";
            }
            PathBuffer[Length] = '\0';
            return std::filesystem::path(PathBuffer).parent_path().string();
#endif
        }
    }
    std::string ShellQuote(const std::string& Value)
    {
#if defined(_WIN32) || defined(__CYGWIN__)
        std::string Result = "\"";
        for (char Ch : Value) {
            if (Ch == '"') {
                Result += "\"\"";
            } else {
                Result += Ch;
            }
        }
        Result += '"';
        return Result;
#else
        std::string Result = "'";
        for (char Ch : Value) {
            if (Ch == '\'') {
                Result += "'\\''";
            } else {
                Result += Ch;
            }
        }
        Result += '\'';
        return Result;
#endif
    }

    std::string MakeTempFilePath(const std::string& Prefix,
                                 const std::string& Extension)
    {
        const auto Now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path TempDir;

#if defined(_WIN32) || defined(__CYGWIN__)
        // Cygwin 的 std::filesystem 默认返回 /tmp 这类 POSIX 路径；原生 Windows 子进程无法解析。
        // 统一通过 Win32 API 获取 DOS 路径，保证写文件端与 curl.exe 看到的是同一个文件。
        char TempPathBuffer[32768] = {};
        const DWORD TempPathLength = GetTempPathA(static_cast<DWORD>(sizeof(TempPathBuffer)),
                                                   TempPathBuffer);
        if (TempPathLength > 0 && TempPathLength < sizeof(TempPathBuffer)) {
            TempDir = std::filesystem::path(TempPathBuffer);
        } else {
            const std::string ExecutableDirectory = GetExecutableDirectory();
            TempDir = ExecutableDirectory.empty()
                ? std::filesystem::current_path()
                : std::filesystem::path(ExecutableDirectory);
        }
#else
        TempDir = std::filesystem::temp_directory_path();
#endif

        std::ostringstream Name;
        Name << Prefix << "_" << Now << "_" << rand() << Extension;
        return (TempDir / Name.str()).string();
    }

    bool WriteTextFile(const std::filesystem::path& Path,
                       const std::string& Content)
    {
        std::ofstream File(Path, std::ios::binary);
        if (!File.is_open()) return false;
        File.write(Content.data(), static_cast<std::streamsize>(Content.size()));
        return File.good();
    }

    bool WriteBinaryFile(const std::filesystem::path& Path,
                         const std::string& Content)
    {
        std::ofstream File(Path, std::ios::binary);
        if (!File.is_open()) return false;
        File.write(Content.data(), static_cast<std::streamsize>(Content.size()));
        return File.good();
    }

    bool ReadFileToString(const std::filesystem::path& Path,
                          std::string& OutContent)
    {
        std::ifstream File(Path, std::ios::binary | std::ios::ate);
        if (!File.is_open()) return false;

        const std::streamsize Size = File.tellg();
        if (Size < 0) return false;

        File.seekg(0, std::ios::beg);
        OutContent.assign(static_cast<size_t>(Size), '\0');
        if (Size > 0 && !File.read(&OutContent[0], Size)) return false;
        return true;
    }

    int NormalizeProcessExitCode(int Status)
    {
#if defined(_WIN32) || defined(__CYGWIN__)
        return Status;
#else
        if (Status == -1) return -1;
        if (WIFEXITED(Status)) return WEXITSTATUS(Status);
        if (WIFSIGNALED(Status)) return 128 + WTERMSIG(Status);
        return Status;
#endif
    }

    std::string ResolveExecutablePath(const std::string& ExecutablePath)
    {
        if (ExecutablePath.empty()) return ExecutablePath;

#if defined(_WIN32) || defined(__CYGWIN__)
        // 显式路径必须尊重管理员配置，不得静默回退到另一份同名程序。
        if (IsAbsolutePathString(ExecutablePath) ||
            ExecutablePath.find('/') != std::string::npos ||
            ExecutablePath.find('\\') != std::string::npos) {
            return ExecutablePath;
        }

        char ResolvedPath[32768] = {};
        const DWORD ResolvedLength = SearchPathA(nullptr,
                                                 ExecutablePath.c_str(),
                                                 ".exe",
                                                 static_cast<DWORD>(sizeof(ResolvedPath)),
                                                 ResolvedPath,
                                                 nullptr);
        if (ResolvedLength > 0 && ResolvedLength < sizeof(ResolvedPath)) {
            return std::string(ResolvedPath, ResolvedLength);
        }

        // 服务账号的 PATH 可能与交互式终端不同；系统 curl 位于 System32 时仍应可用。
        char SystemDirectory[32768] = {};
        const UINT SystemDirectoryLength = GetSystemDirectoryA(SystemDirectory,
                                                                static_cast<UINT>(sizeof(SystemDirectory)));
        if (SystemDirectoryLength > 0 && SystemDirectoryLength < sizeof(SystemDirectory)) {
            std::filesystem::path Candidate = std::filesystem::path(SystemDirectory) / ExecutablePath;
            if (!Candidate.has_extension()) {
                Candidate += ".exe";
            }

            std::error_code ErrorCode;
            if (std::filesystem::is_regular_file(Candidate, ErrorCode)) {
                return Candidate.string();
            }
        }
#endif

        return ExecutablePath;
    }

    bool ReadPipeAll(const std::string& Command,
                     std::string& OutText,
                     int& OutCode,
                     std::string* OutStartError)
    {
        OutText.clear();
        OutCode = -1;
        if (OutStartError) OutStartError->clear();

#if defined(_WIN32) || defined(__CYGWIN__)
        // Cygwin 的 popen() 强依赖部署环境中的 /bin/sh；纯 Windows 发布包通常不包含该文件。
        // 直接使用 Win32 管道与 CreateProcess 可避免 shell 依赖，同时保留现有命令字符串接口。
        SECURITY_ATTRIBUTES SecurityAttributes{};
        SecurityAttributes.nLength = sizeof(SecurityAttributes);
        SecurityAttributes.bInheritHandle = TRUE;

        HANDLE ReadHandle = nullptr;
        HANDLE WriteHandle = nullptr;
        if (!CreatePipe(&ReadHandle, &WriteHandle, &SecurityAttributes, 0)) {
            if (OutStartError) {
                *OutStartError = "CreatePipe failed; win32Error=" + std::to_string(GetLastError());
            }
            return false;
        }

        if (!SetHandleInformation(ReadHandle, HANDLE_FLAG_INHERIT, 0)) {
            const DWORD Win32Error = GetLastError();
            CloseHandle(ReadHandle);
            CloseHandle(WriteHandle);
            if (OutStartError) {
                *OutStartError = "SetHandleInformation failed; win32Error=" + std::to_string(Win32Error);
            }
            return false;
        }

        STARTUPINFOA StartupInfo{};
        StartupInfo.cb = sizeof(StartupInfo);
        StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        StartupInfo.hStdOutput = WriteHandle;
        StartupInfo.hStdError = WriteHandle;
        StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION ProcessInfo{};
        std::vector<char> MutableCommand(Command.begin(), Command.end());
        MutableCommand.push_back('\0');

        const BOOL bCreated = CreateProcessA(nullptr,
                                             MutableCommand.data(),
                                             nullptr,
                                             nullptr,
                                             TRUE,
                                             CREATE_NO_WINDOW,
                                             nullptr,
                                             nullptr,
                                             &StartupInfo,
                                             &ProcessInfo);
        const DWORD CreateError = bCreated ? ERROR_SUCCESS : GetLastError();
        CloseHandle(WriteHandle);

        if (!bCreated) {
            CloseHandle(ReadHandle);
            if (OutStartError) {
                *OutStartError = "CreateProcess failed; win32Error=" + std::to_string(CreateError);
            }
            return false;
        }

        char Buffer[4096];
        DWORD BytesRead = 0;
        while (ReadFile(ReadHandle, Buffer, static_cast<DWORD>(sizeof(Buffer)), &BytesRead, nullptr) &&
               BytesRead > 0) {
            OutText.append(Buffer, BytesRead);
        }
        CloseHandle(ReadHandle);

        WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
        DWORD ProcessExitCode = static_cast<DWORD>(-1);
        if (GetExitCodeProcess(ProcessInfo.hProcess, &ProcessExitCode)) {
            OutCode = static_cast<int>(ProcessExitCode);
        }
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
        return true;
#else
        errno = 0;
        FILE* Pipe = popen(Command.c_str(), "r");
        if (!Pipe) {
            if (OutStartError) {
                std::ostringstream Error;
                Error << "pipe process creation failed";
                if (errno != 0) {
                    Error << "; errno=" << errno << " (" << std::strerror(errno) << ")";
                }
                *OutStartError = Error.str();
            }
            return false;
        }

        char Buffer[4096];
        while (fgets(Buffer, sizeof(Buffer), Pipe)) {
            OutText += Buffer;
        }

        OutCode = NormalizeProcessExitCode(pclose(Pipe));
        return true;
#endif
    }

    std::string MaskSensitiveValue(const std::string& Value)
    {
        if (Value.empty()) return std::string();
        if (Value.size() <= 4) return std::string(Value.size(), '*');

        // 仅保留首尾各两个字节，避免前端响应暴露完整密钥，同时保留最低限度的辨识能力。
        return Value.substr(0, 2)
            + std::string(Value.size() - 4, '*')
            + Value.substr(Value.size() - 2);
    }

    std::string Trim(const std::string& Value)
    {
        size_t Begin = 0;
        while (Begin < Value.size() && std::isspace(static_cast<unsigned char>(Value[Begin]))) {
            ++Begin;
        }

        size_t End = Value.size();
        while (End > Begin && std::isspace(static_cast<unsigned char>(Value[End - 1]))) {
            --End;
        }

        return Value.substr(Begin, End - Begin);
    }

    std::string ToLowerAscii(std::string Value)
    {
        std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Ch) {
            return static_cast<char>(std::tolower(Ch));
        });
        return Value;
    }

    std::string DecodeXmlEntities(const std::string& Value)
    {
        std::string Result;
        Result.reserve(Value.size());

        for (size_t Index = 0; Index < Value.size(); ++Index) {
            const char Ch = Value[Index];
            if (Ch != '&') {
                Result.push_back(Ch);
                continue;
            }

            const size_t Remaining = Value.size() - Index;
            if (Remaining >= 5 && Value.compare(Index + 1, 4, "amp;") == 0) {
                Result.push_back('&');
                Index += 4;
                continue;
            }
            if (Remaining >= 4 && Value.compare(Index + 1, 3, "lt;") == 0) {
                Result.push_back('<');
                Index += 3;
                continue;
            }
            if (Remaining >= 4 && Value.compare(Index + 1, 3, "gt;") == 0) {
                Result.push_back('>');
                Index += 3;
                continue;
            }
            if (Remaining >= 6 && Value.compare(Index + 1, 5, "quot;") == 0) {
                Result.push_back('"');
                Index += 5;
                continue;
            }
            if (Remaining >= 6 && Value.compare(Index + 1, 5, "apos;") == 0) {
                Result.push_back('\'');
                Index += 5;
                continue;
            }

            Result.push_back(Ch);
        }

        return Result;
    }

    std::string ResolveRuntimePath(const std::string& PathValue,
                                   const std::string& FallbackBaseDir)
    {
        namespace fs = std::filesystem;

        if (PathValue.empty() || IsAbsolutePathString(PathValue)) {
            return PathValue;
        }

        const std::string ExecutableDirectory = GetExecutableDirectory();
        if (!ExecutableDirectory.empty()) {
            fs::path Current = ExecutableDirectory;
            int MaxWalk = 10;
            while (MaxWalk-- > 0) {
                std::error_code ec;
                const fs::path ProjectSourceMarker = Current / "yuanbook" / "YuanBook.cpp";
                if (fs::exists(ProjectSourceMarker, ec)) {
                    const fs::path ProjectCandidate = Current / PathValue;
                    if (fs::exists(ProjectCandidate, ec)) {
                        return fs::absolute(ProjectCandidate).string();
                    }
                    break;
                }

                const fs::path Parent = Current.parent_path();
                if (Parent == Current) {
                    break;
                }
                Current = Parent;
            }
        }

        {
            std::error_code ec;
            if (fs::exists(PathValue, ec)) {
                return fs::absolute(PathValue).string();
            }
        }

        if (!ExecutableDirectory.empty()) {
            fs::path Current = ExecutableDirectory;
            int MaxWalk = 10;
            while (MaxWalk-- > 0) {
                std::error_code ec;
                const fs::path Candidate = Current / PathValue;
                if (fs::exists(Candidate, ec)) {
                    return fs::absolute(Candidate).string();
                }

                const fs::path WwwMarker = Current / "www";
                const fs::path DbMarker = Current / "ledger.db";
                const fs::path SourceMarker = Current / "cpHomeCenter.cpp";
                if (fs::exists(WwwMarker, ec) || fs::exists(DbMarker, ec) || fs::exists(SourceMarker, ec)) {
                    return fs::absolute(Current / PathValue).string();
                }

                const fs::path Parent = Current.parent_path();
                if (Parent == Current) {
                    break;
                }
                Current = Parent;
            }
        }

        if (!FallbackBaseDir.empty()) {
            std::error_code ec;
            const fs::path FallbackCandidate = fs::path(FallbackBaseDir) / PathValue;
            if (fs::exists(FallbackCandidate, ec)) {
                return fs::absolute(FallbackCandidate).string();
            }
            return fs::absolute(FallbackCandidate).string();
        }

        return PathValue;
    }
}
