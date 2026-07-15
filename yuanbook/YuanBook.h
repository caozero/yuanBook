// YuanBook.h: YuanBook C++ 应用级封装入口。
//
// 该头文件参考 cpHomeCenter.h 的轻量组织方式，但将应用启动、路径解析、
// Web 服务、账本、语音记账、账单导入、工作日志等现有模块收敛到一个
// 可复用的 FYuanBookApplication 类中，便于 Windows 与 Linux/树莓派复用。

#pragma once

#include "WebServer.h"

#include <cstdint>
#include <string>
#include <vector>

struct FYuanBookAppConfig
{
    std::string ListenIP = "0.0.0.0";
    uint16_t    HttpPort = 5080;
    uint16_t    WsPort = 5081;
    bool        bHttpPortFromCommandLine = false;
    bool        bWsPortFromCommandLine = false;
    bool        bEnableWebSocket = false;

    std::string WwwDir = "./www";
    std::string LedgerDbPath = "./ledger.db";
    std::string LogDir = "./log";
    bool        bResetDatabase = false;

    /** 通过命令行创建的新用户名；空字符串表示未指定创建操作。 */
    std::string AddUser;
    /** 通过命令行修改的已有用户名；空字符串表示未指定修改目标。 */
    std::string TargetUser;
    /**
     * 命令行提供的原始密码，不裁剪首尾空白。
     * 仅当 bPasswordSpecified 为 true 时具有业务意义；空值表示清空密码或创建空密码用户。
     */
    std::string Password;
    /** 是否显式提供了 -pw= 参数，用于区分“未修改密码”和“明确清空密码”。 */
    bool bPasswordSpecified = false;
    std::vector<std::string> AddPermissions;
    std::vector<std::string> RemovePermissions;

    int         MaxSessionsPerUser = 5;

    bool        bVoiceLedgerEnabled = true;
    std::string VoiceLedgerApiKey;
    std::string VoiceLedgerEndpoint = "https://api.deepseek.com/chat/completions";
    std::string VoiceLedgerApiModel = "deepseek-chat";
    std::string VoiceLedgerCurlPath = "curl";
    int         VoiceLedgerTimeoutSec = 60;
    double      VoiceLedgerTemperature = 0.1;
    int         VoiceLedgerMaxTokens = 512;
};

class FYuanBookApplication
{
public:
    FYuanBookApplication();
    ~FYuanBookApplication();

    FYuanBookApplication(const FYuanBookApplication&) = delete;
    FYuanBookApplication& operator=(const FYuanBookApplication&) = delete;

    bool Initialize(const FYuanBookAppConfig& Config);
    bool Start();
    void Stop();
    int RunUntilStopped();

    bool IsRunning() const;

    FWebServer& GetWebServer() { return m_WebServer; }
    const FYuanBookAppConfig& GetConfig() const { return m_Config; }

    static FYuanBookAppConfig MakeDefaultConfig();
    static bool ParseCommandLine(int Argc, char* Argv[], FYuanBookAppConfig& InOutConfig);
    static void PrintHelp(const char* ProgramName);
    static std::string ResolvePath(const std::string& PathValue);

private:
    static FWebServerConfig ToWebServerConfig(const FYuanBookAppConfig& Config);
    void PrintStartupBanner() const;
    void PrintRuntimeConfig() const;

private:
    FYuanBookAppConfig m_Config;
    FWebServer m_WebServer;
    bool m_bInitialized = false;
};
