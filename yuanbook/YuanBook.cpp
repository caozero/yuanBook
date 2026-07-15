// YuanBook.cpp: YuanBook C++ 应用入口与应用级封装实现。
//
// 该文件复用现有 FWebServer、LedgerManager、VoiceLedgerManager、
// BillImportManager、FWorkLogManager 与 SystemUtils 能力，提供统一的
// 跨平台启动入口。平台相关逻辑仅用于信号处理，均通过条件编译隔离。

#include "YuanBook.h"

#include "AuthSecurity.h"
#include "LedgerManager.h"
#include "SystemUtils.h"
#include "WorkLog.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <csignal>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    FYuanBookApplication* GApplication = nullptr;

#ifdef _WIN32
    BOOL WINAPI YuanBookConsoleCtrlHandler(DWORD CtrlType)
    {
        if (CtrlType == CTRL_C_EVENT || CtrlType == CTRL_BREAK_EVENT ||
            CtrlType == CTRL_CLOSE_EVENT || CtrlType == CTRL_SHUTDOWN_EVENT) {
            std::cout << "\n[YuanBook] Stop signal received, shutting down..." << std::endl;
            if (GApplication) {
                GApplication->Stop();
            }
            return TRUE;
        }
        return FALSE;
    }
#else
    void YuanBookSignalHandler(int Signal)
    {
        std::cout << "\n[YuanBook] Signal " << Signal << " received, shutting down..." << std::endl;
        if (GApplication) {
            GApplication->Stop();
        }
    }
#endif

    bool ParseUInt16(const std::string& Text, uint16_t& OutValue)
    {
        if (Text.empty()) return false;
        char* End = nullptr;
        const long Value = std::strtol(Text.c_str(), &End, 10);
        if (!End || *End != '\0' || Value <= 0 || Value > 65535) return false;
        OutValue = static_cast<uint16_t>(Value);
        return true;
    }

    bool ParseInt(const std::string& Text, int& OutValue)
    {
        if (Text.empty()) return false;
        char* End = nullptr;
        const long Value = std::strtol(Text.c_str(), &End, 10);
        if (!End || *End != '\0') return false;
        OutValue = static_cast<int>(Value);
        return true;
    }

    bool ParseDouble(const std::string& Text, double& OutValue)
    {
        if (Text.empty()) return false;
        char* End = nullptr;
        const double Value = std::strtod(Text.c_str(), &End);
        if (!End || *End != '\0') return false;
        OutValue = Value;
        return true;
    }

    bool IsTruthy(const std::string& Text)
    {
        return Text == "1" || Text == "true" || Text == "TRUE" || Text == "yes" || Text == "on";
    }

    bool IsFalsy(const std::string& Text)
    {
        return Text == "0" || Text == "false" || Text == "FALSE" || Text == "no" || Text == "off";
    }

    std::string NormalizePathSeparators(std::string PathValue)
    {
        for (char& Ch : PathValue) {
            if (Ch == '\\') {
                Ch = '/';
            }
        }
        return PathValue;
    }

    std::string TrimCopy(const std::string& Text)
    {
        const auto Begin = std::find_if_not(Text.begin(), Text.end(), [](unsigned char Ch) {
            return std::isspace(Ch) != 0;
        });
        const auto End = std::find_if_not(Text.rbegin(), Text.rend(), [](unsigned char Ch) {
            return std::isspace(Ch) != 0;
        }).base();
        if (Begin >= End) return "";
        return std::string(Begin, End);
    }

    void AppendUniquePermission(std::vector<std::string>& Permissions, const std::string& Permission)
    {
        const std::string Trimmed = TrimCopy(Permission);
        if (Trimmed.empty()) return;
        if (std::find(Permissions.begin(), Permissions.end(), Trimmed) == Permissions.end()) {
            Permissions.push_back(Trimmed);
        }
    }

    void AppendPermissionList(std::vector<std::string>& Permissions, const std::string& Text)
    {
        std::stringstream Stream(Text);
        std::string Item;
        while (std::getline(Stream, Item, ',')) {
            AppendUniquePermission(Permissions, Item);
        }
    }

    bool RemovePermission(std::vector<std::string>& Permissions, const std::string& Permission)
    {
        const std::string Trimmed = TrimCopy(Permission);
        if (Trimmed.empty()) return false;
        const auto OldSize = Permissions.size();
        Permissions.erase(std::remove(Permissions.begin(), Permissions.end(), Trimmed), Permissions.end());
        return Permissions.size() != OldSize;
    }

    std::vector<std::string> ParsePermissionsJsonArray(const std::string& JsonArray)
    {
        std::vector<std::string> Permissions;
        size_t Pos = 0;
        while (true) {
            const size_t BeginQuote = JsonArray.find('"', Pos);
            if (BeginQuote == std::string::npos) break;
            const size_t EndQuote = JsonArray.find('"', BeginQuote + 1);
            if (EndQuote == std::string::npos) break;
            AppendUniquePermission(Permissions, JsonArray.substr(BeginQuote + 1, EndQuote - BeginQuote - 1));
            Pos = EndQuote + 1;
        }
        return Permissions;
    }

    std::string PermissionsToJson(const std::vector<std::string>& Permissions)
    {
        std::ostringstream Oss;
        Oss << "[";
        for (size_t Index = 0; Index < Permissions.size(); ++Index) {
            if (Index > 0) Oss << ",";
            Oss << "\"";
            for (char Ch : Permissions[Index]) {
                if (Ch == '\\' || Ch == '"') Oss << '\\';
                Oss << Ch;
            }
            Oss << "\"";
        }
        Oss << "]";
        return Oss.str();
    }

    std::vector<std::string> ExtractPermissionsFromUserJson(const std::string& UserJson)
    {
        const std::string Key = "\"permissions\":";
        const size_t KeyPos = UserJson.find(Key);
        if (KeyPos == std::string::npos) return {};
        const size_t ArrayBegin = UserJson.find('[', KeyPos + Key.size());
        if (ArrayBegin == std::string::npos) return {};
        const size_t ArrayEnd = UserJson.find(']', ArrayBegin + 1);
        if (ArrayEnd == std::string::npos) return {};
        return ParsePermissionsJsonArray(UserJson.substr(ArrayBegin, ArrayEnd - ArrayBegin + 1));
    }

    bool HasUserManagementCommand(const FYuanBookAppConfig& Config)
    {
        return !Config.AddUser.empty()
            || !Config.TargetUser.empty()
            || Config.bPasswordSpecified
            || !Config.AddPermissions.empty()
            || !Config.RemovePermissions.empty();
    }

    std::tm MakeLocalTime(std::time_t TimeValue)
    {
        std::tm LocalTime{};
#ifdef _WIN32
        localtime_s(&LocalTime, &TimeValue);
#else
        localtime_r(&TimeValue, &LocalTime);
#endif
        return LocalTime;
    }

    std::string MakeBackupTimestamp()
    {
        const std::time_t Now = std::time(nullptr);
        const std::tm LocalTime = MakeLocalTime(Now);
        std::ostringstream Oss;
        Oss << std::put_time(&LocalTime, "%Y-%m-%d_%H-%M-%S");
        return Oss.str();
    }

    bool CopyFileToBackup(const fs::path& SourcePath,
                          const fs::path& BackupPath,
                          std::error_code& OutError)
    {
        OutError.clear();
        fs::copy_file(SourcePath, BackupPath, fs::copy_options::none, OutError);
        return !OutError;
    }

    bool RemoveExistingFile(const fs::path& FilePath, std::error_code& OutError)
    {
        OutError.clear();
        const bool bRemoved = fs::remove(FilePath, OutError);
        return bRemoved && !OutError;
    }

    fs::path ResolveResetDatabasePath(const std::string& DbPathValue)
    {
        const fs::path RawPath = DbPathValue.empty()
            ? fs::path("./ledger.db")
            : fs::path(NormalizePathSeparators(DbPathValue));
        if (RawPath.is_absolute()) {
            return fs::absolute(RawPath);
        }

        std::error_code Error;
        if (fs::exists(RawPath, Error)) {
            return fs::absolute(RawPath);
        }

        Error.clear();
        const fs::path RawParent = RawPath.has_parent_path() ? RawPath.parent_path() : fs::path(".");
        if (fs::exists(RawParent, Error)) {
            return fs::absolute(RawPath);
        }

        return fs::path(FYuanBookApplication::ResolvePath(RawPath.string()));
    }

    int ResetLedgerDatabase(const FYuanBookAppConfig& Config)
    {
        const fs::path DbPath = ResolveResetDatabasePath(Config.LedgerDbPath);
        const fs::path DbDir = DbPath.has_parent_path() ? DbPath.parent_path() : fs::current_path();
        const fs::path BackupDir = DbDir / "bak";
        const std::string Timestamp = MakeBackupTimestamp();

        std::cout << "[YuanBook] Reset requested. Web server will not start." << std::endl;
        std::cout << "[YuanBook] Target database: " << DbPath.string() << std::endl;
        std::cout << "[YuanBook] Backup directory: " << BackupDir.string() << std::endl;

        std::error_code Error;
        fs::create_directories(BackupDir, Error);
        if (Error) {
            std::cerr << "[YuanBook] Failed to create backup directory: "
                      << BackupDir.string() << " : " << Error.message() << std::endl;
            return 1;
        }

        const std::string BaseBackupName = DbPath.filename().string() + "." + Timestamp;
        const fs::path FilesToReset[] = {
            DbPath,
            fs::path(DbPath.string() + "-wal"),
            fs::path(DbPath.string() + "-shm")
        };

        bool bAnyExistingFile = false;
        for (const fs::path& SourcePath : FilesToReset) {
            Error.clear();
            if (!fs::exists(SourcePath, Error)) {
                if (Error) {
                    std::cerr << "[YuanBook] Failed to check file: "
                              << SourcePath.string() << " : " << Error.message() << std::endl;
                    return 1;
                }
                std::cout << "[YuanBook] Skip missing file: " << SourcePath.string() << std::endl;
                continue;
            }

            bAnyExistingFile = true;
            fs::path BackupPath = BackupDir / BaseBackupName;
            if (SourcePath == FilesToReset[1]) {
                BackupPath += "-wal";
            } else if (SourcePath == FilesToReset[2]) {
                BackupPath += "-shm";
            }

            if (!CopyFileToBackup(SourcePath, BackupPath, Error)) {
                std::cerr << "[YuanBook] Failed to backup file: "
                          << SourcePath.string() << " -> " << BackupPath.string()
                          << " : " << Error.message() << std::endl;
                return 1;
            }
            std::cout << "[YuanBook] Backed up: " << SourcePath.string()
                      << " -> " << BackupPath.string() << std::endl;
        }

        for (const fs::path& FilePath : FilesToReset) {
            Error.clear();
            if (!fs::exists(FilePath, Error)) {
                if (Error) {
                    std::cerr << "[YuanBook] Failed to check file before delete: "
                              << FilePath.string() << " : " << Error.message() << std::endl;
                    return 1;
                }
                continue;
            }

            if (!RemoveExistingFile(FilePath, Error)) {
                std::cerr << "[YuanBook] Failed to delete old database file: "
                          << FilePath.string() << " : " << Error.message() << std::endl;
                return 1;
            }
            std::cout << "[YuanBook] Deleted: " << FilePath.string() << std::endl;
        }

        if (!bAnyExistingFile) {
            std::cout << "[YuanBook] No existing database files found; a fresh database will be created." << std::endl;
        }

        LedgerManager Ledger;
        if (!Ledger.Initialize(DbPath.string())) {
            std::cerr << "[YuanBook] Failed to initialize fresh database: "
                      << DbPath.string() << std::endl;
            return 1;
        }
        Ledger.Shutdown();

        std::cout << "[YuanBook] Database reset completed successfully." << std::endl;
        return 0;
    }

    int ExecuteUserManagementCommand(const FYuanBookAppConfig& Config)
    {
        const fs::path DbPath = ResolveResetDatabasePath(Config.LedgerDbPath);
        std::cout << "[YuanBook] User management command requested. Web server will not start." << std::endl;
        std::cout << "[YuanBook] Target database: " << DbPath.string() << std::endl;

        LedgerManager Ledger;
        if (!Ledger.Initialize(DbPath.string())) {
            std::cerr << "[YuanBook] Failed to initialize ledger database: "
                      << DbPath.string() << std::endl;
            return 1;
        }

        bool bOk = true;
        if (!Config.AddUser.empty()) {
            std::vector<std::string> Permissions;
            AppendUniquePermission(Permissions, "user");
            for (const std::string& Permission : Config.AddPermissions) {
                AppendUniquePermission(Permissions, Permission);
            }

            const std::string PermissionsJson = PermissionsToJson(Permissions);
            const std::string PasswordHash = Config.bPasswordSpecified
                ? AuthSecurity::HashPasswordForStorage(Config.Password)
                : std::string();
            if (!Ledger.CreateUser(Config.AddUser, PasswordHash, PermissionsJson)) {
                std::cerr << "[YuanBook] Failed to add user: " << Config.AddUser
                          << " (maybe already exists)" << std::endl;
                bOk = false;
            } else {
                std::cout << "[YuanBook] Added user: " << Config.AddUser
                          << ", permissions=" << PermissionsJson << std::endl;
                if (PasswordHash.empty()) {
                    std::cout << "[YuanBook] Password is empty; the next successful login will initialize it." << std::endl;
                } else {
                    std::cout << "[YuanBook] Initial password has been set." << std::endl;
                }
            }
        }

        if (bOk && !Config.TargetUser.empty()) {
            std::string UserJson;
            if (!Ledger.GetUserByUsername(Config.TargetUser, UserJson)) {
                std::cerr << "[YuanBook] User not found: " << Config.TargetUser << std::endl;
                bOk = false;
            } else {
                if (Config.bPasswordSpecified) {
                    const std::string PasswordHash =
                        AuthSecurity::HashPasswordForStorage(Config.Password);
                    int DeletedSessionCount = 0;
                    if (!Ledger.UpdateUserPasswordAndInvalidateSessions(
                            Config.TargetUser, PasswordHash, DeletedSessionCount)) {
                        std::cerr << "[YuanBook] Failed to update password for user: "
                                  << Config.TargetUser << std::endl;
                        bOk = false;
                    } else {
                        std::cout << "[YuanBook] "
                                  << (PasswordHash.empty() ? "Cleared password" : "Updated password")
                                  << " for user: " << Config.TargetUser
                                  << "; invalidated sessions=" << DeletedSessionCount << std::endl;
                    }
                }

                if (bOk && (!Config.AddPermissions.empty() || !Config.RemovePermissions.empty())) {
                    std::vector<std::string> Permissions = ExtractPermissionsFromUserJson(UserJson);
                    if (Permissions.empty()) {
                        AppendUniquePermission(Permissions, "user");
                    }

                    for (const std::string& Permission : Config.AddPermissions) {
                        AppendUniquePermission(Permissions, Permission);
                    }
                    for (const std::string& Permission : Config.RemovePermissions) {
                        RemovePermission(Permissions, Permission);
                    }

                    const std::string PermissionsJson = PermissionsToJson(Permissions);
                    if (!Ledger.UpdateUserPermissions(Config.TargetUser, PermissionsJson)) {
                        std::cerr << "[YuanBook] Failed to update permissions for user: "
                                  << Config.TargetUser << std::endl;
                        bOk = false;
                    } else {
                        std::cout << "[YuanBook] Updated permissions for user: " << Config.TargetUser
                                  << ", permissions=" << PermissionsJson << std::endl;
                    }
                }
            }
        }

        Ledger.Shutdown();
        return bOk ? 0 : 1;
    }
}

FYuanBookApplication::FYuanBookApplication()
    : m_Config(MakeDefaultConfig())
{
}

FYuanBookApplication::~FYuanBookApplication()
{
    Stop();
}

FYuanBookAppConfig FYuanBookApplication::MakeDefaultConfig()
{
    return FYuanBookAppConfig{};
}

std::string FYuanBookApplication::ResolvePath(const std::string& PathValue)
{
    return SystemUtils::ResolveRuntimePath(PathValue);
}

FWebServerConfig FYuanBookApplication::ToWebServerConfig(const FYuanBookAppConfig& Config)
{
    FWebServerConfig WebConfig;
    WebConfig.ListenIP = Config.ListenIP;
    WebConfig.HttpPort = Config.HttpPort;
    WebConfig.WsPort = Config.WsPort > 0 ? Config.WsPort : static_cast<uint16_t>(Config.HttpPort + 1);
    WebConfig.bHttpPortFromCommandLine = Config.bHttpPortFromCommandLine;
    WebConfig.bWsPortFromCommandLine = Config.bWsPortFromCommandLine;
    WebConfig.bEnableWebSocket = Config.bEnableWebSocket;
    WebConfig.WwwDir = Config.WwwDir;
    WebConfig.LedgerDbPath = Config.LedgerDbPath;
    WebConfig.MaxSessionsPerUser = Config.MaxSessionsPerUser;
    WebConfig.bVoiceLedgerEnabled = Config.bVoiceLedgerEnabled;
    WebConfig.VoiceLedgerApiKey = Config.VoiceLedgerApiKey;
    WebConfig.VoiceLedgerEndpoint = Config.VoiceLedgerEndpoint;
    WebConfig.VoiceLedgerApiModel = Config.VoiceLedgerApiModel;
    WebConfig.VoiceLedgerCurlPath = Config.VoiceLedgerCurlPath;
    WebConfig.VoiceLedgerTimeoutSec = Config.VoiceLedgerTimeoutSec;
    WebConfig.VoiceLedgerTemperature = Config.VoiceLedgerTemperature;
    WebConfig.VoiceLedgerMaxTokens = Config.VoiceLedgerMaxTokens;
    return WebConfig;
}

bool FYuanBookApplication::Initialize(const FYuanBookAppConfig& Config)
{
    m_Config = Config;
    m_Config.WwwDir = ResolvePath(m_Config.WwwDir);
    m_Config.LedgerDbPath = ResolvePath(m_Config.LedgerDbPath);
    m_Config.LogDir = ResolvePath(m_Config.LogDir);

    if (!FWorkLogManager::Get().Initialize(m_Config.LogDir, 0)) {
        std::cerr << "[YuanBook] Failed to initialize work log: " << m_Config.LogDir << std::endl;
        return false;
    }
    FWorkLogManager::Get().InstallStreamRedirects();

    m_bInitialized = true;
    return true;
}

bool FYuanBookApplication::Start()
{
    if (!m_bInitialized) {
        if (!Initialize(m_Config)) {
            return false;
        }
    }

    PrintStartupBanner();
    PrintRuntimeConfig();

    FWebServerConfig WebConfig = ToWebServerConfig(m_Config);
    if (!m_WebServer.Start(WebConfig)) {
        std::cerr << "[YuanBook] Failed to start WebServer" << std::endl;
        FWorkLogManager::Get().Shutdown();
        return false;
    }

    std::cout << "[YuanBook] Web server started: http://localhost:"
              << m_Config.HttpPort << std::endl;
    return true;
}

void FYuanBookApplication::Stop()
{
    if (m_WebServer.IsRunning()) {
        m_WebServer.Stop();
    }

    if (m_bInitialized) {
        FWorkLogManager::Get().Shutdown();
        m_bInitialized = false;
    }
}

int FYuanBookApplication::RunUntilStopped()
{
    while (IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

bool FYuanBookApplication::IsRunning() const
{
    return m_WebServer.IsRunning();
}

void FYuanBookApplication::PrintStartupBanner() const
{
    std::cout << "========================================" << std::endl;
    std::cout << "  YuanBook - Cross-platform Ledger App" << std::endl;
    std::cout << "========================================" << std::endl;
}

void FYuanBookApplication::PrintRuntimeConfig() const
{
    std::cout << std::endl;
    std::cout << "--- Runtime Config ---" << std::endl;
    std::cout << "  Listen IP:     " << m_Config.ListenIP << std::endl;
    std::cout << "  HTTP Port:     " << m_Config.HttpPort << std::endl;
    std::cout << "  Web Root:      " << m_Config.WwwDir << std::endl;
    std::cout << "  Ledger DB:     " << m_Config.LedgerDbPath << std::endl;
    std::cout << "  Log Dir:       " << m_Config.LogDir << std::endl;
    std::cout << "  Data Channel:  POST /api/channel and POST /data" << std::endl;
    std::cout << "  Voice Ledger:  " << (m_Config.bVoiceLedgerEnabled ? "enabled" : "disabled") << std::endl;
    std::cout << std::endl;
}

void FYuanBookApplication::PrintHelp(const char* ProgramName)
{
    const char* Name = ProgramName && ProgramName[0] ? ProgramName : "YuanBook";
    std::cout << "用法: " << Name << " [选项]" << std::endl;
    std::cout << std::endl;
    std::cout << "  服务设置:" << std::endl;
    std::cout << "    -listen=IP          设置监听 IP（默认值: 0.0.0.0）" << std::endl;
    std::cout << "    -http=N             设置 HTTP 监听端口（默认值: 5080）" << std::endl;
    std::cout << "    -www=PATH           设置 Web 静态资源目录（默认值: ./www）" << std::endl;
    std::cout << "    -db=PATH            设置账本 SQLite 数据库路径（默认值: ./ledger.db）" << std::endl;
    std::cout << "    -log=PATH           设置工作日志目录（默认值: ./log）" << std::endl;
    std::cout << "    -reset              备份并重置账本数据库，然后退出程序" << std::endl;
    std::cout << "    -max-sessions=N     设置每个用户的最大会话数，N <= 0 表示不限制" << std::endl;
    std::cout << std::endl;
    std::cout << "  用户管理（执行操作后直接退出，不启动服务）:" << std::endl;
    std::cout << "    -addUser=NAME       添加新用户；未指定 -pw 时密码为空" << std::endl;
    std::cout << "    -user=NAME          选择需要修改密码或权限的已有用户" << std::endl;
    std::cout << "    -pw=PASSWORD        设置密码；空值表示清空并由用户下次登录时设置" << std::endl;
    std::cout << "    -p=PERMS            添加权限，支持使用逗号分隔多个权限" << std::endl;
    std::cout << "    -unp=PERMS          移除权限，支持使用逗号分隔多个权限" << std::endl;
    std::cout << "                       示例: -addUser=alice -pw=secret -p=admin" << std::endl;
    std::cout << "                             -user=alice -pw=newSecret" << std::endl;
    std::cout << "                             -user=alice -pw=" << std::endl;
    std::cout << "                             -user=alice -p=admin,viewer" << std::endl;
    std::cout << std::endl;
    std::cout << "  语音记账与账单导入 AI 分类:" << std::endl;
    std::cout << "    -voice=true|false  启用或禁用语音记账" << std::endl;
    std::cout << "    -voice-key=KEY     设置 DeepSeek API Key" << std::endl;
    std::cout << "    -voice-endpoint=U  设置 DeepSeek 兼容接口地址" << std::endl;
    std::cout << "    -voice-model=NAME  设置模型名称（默认值: deepseek-chat）" << std::endl;
    std::cout << "    -curl=PATH         设置 curl 可执行文件路径（默认值: curl）" << std::endl;
    std::cout << "    -voice-timeout=N   设置请求超时时间，单位为秒" << std::endl;
    std::cout << "    -voice-temp=N      设置模型 temperature 参数" << std::endl;
    std::cout << "    -voice-tokens=N    设置模型响应的最大 token 数" << std::endl;
    std::cout << std::endl;
    std::cout << "  -h, --help           显示此帮助说明" << std::endl;
}

bool FYuanBookApplication::ParseCommandLine(int Argc, char* Argv[], FYuanBookAppConfig& InOutConfig)
{
    for (int Index = 1; Index < Argc; ++Index) {
        const std::string Arg = Argv[Index] ? Argv[Index] : "";

        if (Arg == "-h" || Arg == "--help") {
            PrintHelp(Argc > 0 ? Argv[0] : "YuanBook");
            return false;
        } else if (Arg.rfind("-listen=", 0) == 0) {
            InOutConfig.ListenIP = Arg.substr(8);
        } else if (Arg.rfind("-http=", 0) == 0) {
            if (!ParseUInt16(Arg.substr(6), InOutConfig.HttpPort)) return false;
            InOutConfig.WsPort = static_cast<uint16_t>(InOutConfig.HttpPort + 1);
            InOutConfig.bHttpPortFromCommandLine = true;
        } else if (Arg.rfind("-www=", 0) == 0) {
            InOutConfig.WwwDir = Arg.substr(5);
        } else if (Arg.rfind("-db=", 0) == 0) {
            InOutConfig.LedgerDbPath = Arg.substr(4);
        } else if (Arg.rfind("-log=", 0) == 0) {
            InOutConfig.LogDir = Arg.substr(5);
        } else if (Arg == "-reset") {
            InOutConfig.bResetDatabase = true;
        } else if (Arg.rfind("-addUser=", 0) == 0) {
            InOutConfig.AddUser = TrimCopy(Arg.substr(9));
        } else if (Arg.rfind("-user=", 0) == 0) {
            InOutConfig.TargetUser = TrimCopy(Arg.substr(6));
        } else if (Arg.rfind("-pw=", 0) == 0) {
            // 密码属于不透明数据，禁止 Trim；空值具有清空密码的明确业务语义。
            InOutConfig.Password = Arg.substr(4);
            InOutConfig.bPasswordSpecified = true;
        } else if (Arg.rfind("-p=", 0) == 0) {
            AppendPermissionList(InOutConfig.AddPermissions, Arg.substr(3));
        } else if (Arg.rfind("-unp=", 0) == 0) {
            AppendPermissionList(InOutConfig.RemovePermissions, Arg.substr(5));
        } else if (Arg.rfind("-max-sessions=", 0) == 0) {
            if (!ParseInt(Arg.substr(14), InOutConfig.MaxSessionsPerUser)) return false;
        } else if (Arg.rfind("-voice=", 0) == 0) {
            const std::string Value = Arg.substr(7);
            if (IsTruthy(Value)) InOutConfig.bVoiceLedgerEnabled = true;
            else if (IsFalsy(Value)) InOutConfig.bVoiceLedgerEnabled = false;
            else return false;
        } else if (Arg.rfind("-voice-key=", 0) == 0) {
            InOutConfig.VoiceLedgerApiKey = Arg.substr(11);
        } else if (Arg.rfind("-voice-endpoint=", 0) == 0) {
            InOutConfig.VoiceLedgerEndpoint = Arg.substr(16);
        } else if (Arg.rfind("-voice-model=", 0) == 0) {
            InOutConfig.VoiceLedgerApiModel = Arg.substr(13);
        } else if (Arg.rfind("-curl=", 0) == 0) {
            InOutConfig.VoiceLedgerCurlPath = Arg.substr(6);
        } else if (Arg.rfind("-voice-timeout=", 0) == 0) {
            if (!ParseInt(Arg.substr(15), InOutConfig.VoiceLedgerTimeoutSec)) return false;
        } else if (Arg.rfind("-voice-temp=", 0) == 0) {
            if (!ParseDouble(Arg.substr(12), InOutConfig.VoiceLedgerTemperature)) return false;
        } else if (Arg.rfind("-voice-tokens=", 0) == 0) {
            if (!ParseInt(Arg.substr(14), InOutConfig.VoiceLedgerMaxTokens)) return false;
        } else {
            std::cerr << "[YuanBook] Unknown option: " << Arg << std::endl;
            PrintHelp(Argc > 0 ? Argv[0] : "YuanBook");
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[])
{
    FYuanBookAppConfig Config = FYuanBookApplication::MakeDefaultConfig();
    if (!FYuanBookApplication::ParseCommandLine(argc, argv, Config)) {
        return 0;
    }

    if (Config.bResetDatabase) {
        return ResetLedgerDatabase(Config);
    }

    if (HasUserManagementCommand(Config)) {
        if (!Config.AddUser.empty() && !Config.TargetUser.empty()) {
            std::cerr << "[YuanBook] -addUser and -user cannot be used together." << std::endl;
            return 1;
        }
        if (Config.AddUser.empty() && Config.TargetUser.empty()) {
            std::cerr << "[YuanBook] User management requires -addUser=NAME or -user=NAME." << std::endl;
            return 1;
        }
        if (!Config.AddUser.empty() && !Config.RemovePermissions.empty()) {
            std::cerr << "[YuanBook] -unp cannot be used with -addUser." << std::endl;
            return 1;
        }
        if (!Config.TargetUser.empty()
            && !Config.bPasswordSpecified
            && Config.AddPermissions.empty()
            && Config.RemovePermissions.empty()) {
            std::cerr << "[YuanBook] -user requires at least one action: -pw, -p or -unp." << std::endl;
            return 1;
        }
        return ExecuteUserManagementCommand(Config);
    }

    FYuanBookApplication App;
    GApplication = &App;

#ifdef _WIN32
    SetConsoleCtrlHandler(YuanBookConsoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, YuanBookSignalHandler);
    std::signal(SIGTERM, YuanBookSignalHandler);
#endif

    if (!App.Initialize(Config)) {
        GApplication = nullptr;
        return 1;
    }

    if (!App.Start()) {
        GApplication = nullptr;
        return 1;
    }

    std::cout << "[YuanBook] Press Ctrl+C to stop..." << std::endl;
    const int Result = App.RunUntilStopped();
    GApplication = nullptr;
    std::cout << "[YuanBook] Exited. Goodbye." << std::endl;
    return Result;
}
