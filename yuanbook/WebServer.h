// WebServer.h — 迷你 Web 服务模块
// 提供 HTTP API + POST 数据通道 + 可选 WebSocket 数据通道 + 登录认证
// 集成家庭记账功能 (LedgerManager)
//
// HTTP:    基于 cpp-httplib (header-only, MIT 协议), 端口默认 8080
// POST:    /api/channel 复用 WebSocket 消息协议, 默认数据通道
// WebSocket: 基于 raw socket (RFC 6455), 端口默认 8081 (HTTP 端口 + 1), 默认关闭
// Auth:    基于 SQLite users 表 + 服务端 token 会话持久化
// Ledger:  家庭记账功能，通过 /api/channel 扩展 ledger.* 命令

#pragma once

// ---------------------------------------------------------------------------
// 平台头文件 (Windows 下必须在 httplib.h 之前包含 winsock)
// ---------------------------------------------------------------------------
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#endif

#include <string>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include "LedgerManager.h"
#include "RuntimeSettings.h"
#include "VoiceLedgerManager.h"
#include "BillImportManager.h"


// ============================================================================
// 前向声明: httplib 类型 (必须在 FWebServer 类之前)
// ============================================================================
namespace httplib {
    class Server;
    struct Request;
    struct Response;
}

// ============================================================================
// 1. Web 服务配置
// ============================================================================
struct FWebServerConfig
{
    // 监听地址与端口
    std::string ListenIP  = "0.0.0.0";
    uint16_t    HttpPort  = 5080;       // HTTP 服务端口
    uint16_t    WsPort    = 5081;       // WebSocket 端口 (默认 HttpPort+1)
    bool        bHttpPortFromCommandLine = false; // HTTP 端口是否由启动参数显式指定
    bool        bWsPortFromCommandLine = false;   // WebSocket 端口是否由启动参数显式指定
    bool        bEnableWebSocket = false; // 是否启用 WebSocket 服务, 默认关闭

    // Web 前端静态资源目录
    std::string WwwDir = "./www";                 // index.html / ledger.html / css / js 所在目录

    // 认证与数据配置
    std::string LedgerDbPath = "./ledger.db";     // 家庭记账数据库文件路径（含认证会话表）
    int         MaxSessionsPerUser = 5;            // 每个用户最多保留的会话数；<=0 表示不限制

    // 语音记账配置（DeepSeek API，通过系统 curl 访问 HTTPS）
    bool        bVoiceLedgerEnabled = true;            // 是否启用语音记账功能
    std::string VoiceLedgerApiKey;                     // DeepSeek API Key，优先从数据库系统参数读取
    std::string VoiceLedgerEndpoint = "https://api.deepseek.com/chat/completions";
    std::string VoiceLedgerApiModel = "deepseek-chat";
    std::string VoiceLedgerCurlPath = "curl";
    int         VoiceLedgerTimeoutSec = 60;
    double      VoiceLedgerTemperature = 0.1;
    int         VoiceLedgerMaxTokens = 512;
};

// ============================================================================
// 2. 用户配置记录
// ============================================================================
struct FAuthUser
{
    std::string Username;      // 用户名
    std::string PasswordHash;  // 密码哈希 (SHA1, hex)
    std::string Permissions;   // 权限字段 (JSON 字符串数组, 如 ["admin"])
};

// ============================================================================
// 3. 登录挑战码记录（Challenge-Response 认证）
// ============================================================================
struct FLoginChallenge
{
    std::string Challenge;   // 随机挑战字符串
    std::string Username;    // 关联的用户名
    int64_t     CreatedAt = 0; // 创建时间戳（Unix 秒），用于超时清理
};

// ============================================================================
// 4. 会话记录
// ============================================================================
struct FAuthSession
{
    std::string Token;       // 随机会话令牌
    std::string Username;    // 用户名
    int64_t     CreatedAt = 0; // 创建时间戳（Unix 秒），用于超出上限时清理最旧会话
    // 注: 不再有过期时间, 登录信息永久有效直至主动登出
};

/**
 * @brief 单次数据通道请求的认证上下文。
 * @details 该对象仅描述当前执行线程正在处理的一个同步请求，不允许跨请求共享。
 *          HTTP 与 WebSocket 入口在完成 Token 校验后写入上下文，所有下游权限检查只读取该上下文，
 *          从而避免并发请求通过服务器实例成员互相覆盖身份。
 */
struct FAuthRequestContext
{
    /** 当前请求携带的会话令牌；未启用认证时允许为空。 */
    std::string Token;
    /** Token 对应的用户名；空字符串表示请求尚未建立有效身份。 */
    std::string Username;
    /** 用户权限 JSON 快照，例如 ["user"]。 */
    std::string Permissions = "[]";
    /** 是否已经通过服务端会话校验。 */
    bool bAuthenticated = false;
};

// ============================================================================
// 5. FWebServer — Web 服务
// ============================================================================
class FWebServer : public IRuntimeSettingObserver
{
public:
    FWebServer();
    ~FWebServer();

    // ---- 生命周期 ----

    // 启动 Web 服务 (非阻塞, 内部创建线程)
    bool Start(const FWebServerConfig& Config);

    // 停止 Web 服务
    void Stop();

    // 是否正在运行
    bool IsRunning() const { return m_bRunning.load(); }

    // WebSocket 监听状态 (供 WS 线程设置)
    void SetWsListening(bool b) { m_bWsListening.store(b); }
    bool IsWsListening() const { return m_bWsListening.load(); }

    // ---- WebSocket 消息处理 (公开接口, 供 WS 线程调用) ----
    using WS_SendFunc = std::function<bool(const std::string&)>;
    void HandleWSMessage(const std::string& MsgJson, WS_SendFunc Send);

    // ---- 认证相关公开接口 ----
    bool IsTokenValid(const std::string& Token) const;
    std::string GetUserPermissions(const std::string& Username) const;

    // ---- 运行时系统参数观察者 ----
    /**
     * @brief 获取观察者名称，用于 RuntimeSettingManager 日志与调试定位。
     * @return 固定返回 WebServer。
     */
    std::string GetObserverName() const override;

    /**
     * @brief 判断 WebServer 是否负责处理指定系统参数。
     * @param Key 参数键。
     * @return true 表示该参数涉及 Web/Auth/Voice 运行态，需要由 WebServer 应用。
     */
    bool CanHandleSetting(const std::string& Key) const override;

    /**
     * @brief 响应系统参数变更并应用到 Web/Auth/Voice 子系统。
     * @param Event 参数变更事件，包含最终有效值与生效模式。
     * @param Manager 运行时参数管理器，可读取完整快照以重建复合配置。
     * @return 当前运行实例的应用结果。
     * @sideeffect 可能更新 m_Config、裁剪认证会话、热更新 VoiceLedgerManager 配置。
     */
    FRuntimeSettingApplyResult OnRuntimeSettingChanged(
        const FRuntimeSettingChangeEvent& Event,
        const RuntimeSettingManager& Manager) override;

    // ---- 未来预留: 回调设置 ----
    // void SetClientRegisterCallback(FClientRegisterCallback Callback);
    // void SetClientStatusCallback(FClientStatusCallback Callback);

    // ---- 公开成员: 账本管理器 ----
    LedgerManager m_LedgerManager;

    // ---- 公开成员: 语音记账管理器 ----
    VoiceLedgerManager m_VoiceLedgerManager;

    // ---- 公开成员: 账单导入管理器 ----
    BillImportManager m_BillImportManager;

private:
    // ---- 路由注册 (HTTP) ----
    void SetupRoutes();

    // ---- 记账命令处理 ----
    void HandleLedgerUserList(const std::string& DataStr,
                              const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerUserCreate(const std::string& DataStr,
                                const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerUserUpdatePwd(const std::string& DataStr,
                                    const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerUserSetActive(const std::string& DataStr,
                                   const std::string& Cmd, WS_SendFunc Send);
    /** @brief 返回当前认证用户创建的全部 PID 账本绑定。 */
    void HandleLedgerPidList(const std::string& DataStr,
                             const std::string& Cmd, WS_SendFunc Send);
    /** @brief 创建一个必须显式绑定账本的全局唯一 PID。 */
    void HandleLedgerPidCreate(const std::string& DataStr,
                               const std::string& Cmd, WS_SendFunc Send);
    /** @brief 修改当前用户所拥有 PID 的目标账本。 */
    void HandleLedgerPidUpdateLedger(const std::string& DataStr,
                                     const std::string& Cmd, WS_SendFunc Send);
    /** @brief 删除当前用户所拥有的 PID。 */
    void HandleLedgerPidDelete(const std::string& DataStr,
                               const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerUserProfile(const std::string& DataStr,
                                  const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerUserDelete(const std::string& DataStr,
                                 const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupList(const std::string& DataStr,
                               const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupCreate(const std::string& DataStr,
                                  const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupDelete(const std::string& DataStr,
                                  const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupSetCurrent(const std::string& DataStr,
                                      const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupMembers(const std::string& DataStr,
                                   const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupAddMember(const std::string& DataStr,
                                     const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupInviteMember(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupIncomingInvites(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupSentInvites(const std::string& DataStr,
                                      const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupAcceptInvite(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupRejectInvite(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupCancelInvite(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerGroupRemoveMember(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerCategoryList(const std::string& DataStr,
                                   const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerCategoryCreate(const std::string& DataStr,
                                     const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerCategoryUpdate(const std::string& DataStr,
                                     const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerCategoryDelete(const std::string& DataStr,
                                     const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerTransactionCreate(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerTransactionList(const std::string& DataStr,
                                      const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerTransactionUpdate(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerTransactionDelete(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerTransactionStats(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerVoiceCreate(const std::string& DataStr,
                                  const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerVoiceTest(const std::string& DataStr,
                                const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerVoiceParse(const std::string& DataStr,
                                 const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerInviteCreate(const std::string& DataStr,
                                  const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerInviteCurrent(const std::string& DataStr,
                                   const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerBillImportStatus(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerSystemSettingsList(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerSystemSettingsUpdate(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send);
    void HandleLedgerSystemSettingsDelete(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send);

    // ---- 系统参数运行时快照与应用 ----
    FRuntimeSettingApplyResult NotifySystemSettingChanged(const FSystemSettingChangedEvent& Event,
                                                          bool bDeleted);
    void ReloadRuntimeConfigFromDatabase();
    void ApplySystemSettingValue(const std::string& SettingKey,
                                 const std::string& SettingValue);
    FVoiceLedgerDeepSeekConfig BuildVoiceLedgerConfigFromRuntimeSettings(const RuntimeSettingManager& Manager) const;
    void ApplyVoiceLedgerRuntimeConfig(const RuntimeSettingManager& Manager);
    int PruneAllUsersOldSessions();

    // ---- HTTP 数据通道处理 ----
    void HandleChannel(const struct httplib::Request& Req,
                       struct httplib::Response& Res);
    void HandleDataChannel(const struct httplib::Request& Req,
                           struct httplib::Response& Res);
    void HandleUpload(const struct httplib::Request& Req,
                      struct httplib::Response& Res);
    /**
     * @brief 解析用户当前账本上下文。
     * @param Username 当前登录用户名。
     * @return 有效当前账本 ID；无可用账本时返回 0。
     * @note 该结果只用于请求未显式携带 ledgerId 时的 UI 缺省选择，不能替代领域层授权。
     */
    int ResolveDefaultLedgerIdForUser(const std::string& Username);
    /**
     * @brief 获取当前执行线程所处理请求的用户名。
     * @param OutUsername 输出当前请求用户名；无有效身份时输出空字符串。
     * @return 存在有效用户名时返回 true。
     * @note 请求上下文按线程隔离，禁止将返回结果缓存到服务器实例级共享状态。
     */
    bool GetCurrentRequestUsername(std::string& OutUsername) const;
    bool CurrentRequestHasPermission(const std::string& PermissionName,
                                     std::string* OutUsername = nullptr) const;
    bool EnsurePermissionForRequest(const std::string& PermissionName,
                                    const std::string& Cmd,
                                    WS_SendFunc Send,
                                    std::string* OutUsername = nullptr) const;
    bool EnsureAdminForRequest(const std::string& Cmd,
                               WS_SendFunc Send,
                               std::string* OutUsername = nullptr) const;
    bool EnsureUserForRequest(const std::string& Cmd,
                              WS_SendFunc Send,
                              std::string* OutUsername = nullptr) const;
    bool EnsureGroupMemberForRequest(int GroupId, const std::string& Cmd,
                                     WS_SendFunc Send,
                                     std::string* OutUsername = nullptr);

    // ---- 认证相关方法 ----
    bool LoadUsersFromDatabase();
    bool LoadSessionsFromDatabase();
    bool SaveSessionToDatabase(const FAuthSession& Session);
    bool DeleteSessionsFromDatabase(const std::vector<std::string>& Tokens);
    /**
     * @brief 从运行时会话缓存移除指定用户的全部 Token。
     * @param Username 目标用户名。
     * @return 实际移除的内存会话数量。
     * @note 数据库事务必须在调用本函数之前完成，避免持有会话锁时进入数据库层。
     */
    int RemoveUserSessionsFromMemory(const std::string& Username);
    int  PruneOldSessionsForUserLocked(const std::string& Username,
                                        const std::string& KeepToken = "");
    std::string GenerateToken() const;
    std::string HashPassword(const std::string& Password) const;

    // ---- HTTP 认证接口处理 ----
    void HandleLoginPrecheck(const struct httplib::Request& Req,
                             struct httplib::Response& Res);
    void HandleLoginChallenge(const struct httplib::Request& Req,
                              struct httplib::Response& Res);
    void HandleLogin(const struct httplib::Request& Req,
                     struct httplib::Response& Res);
    void HandleLogout(const struct httplib::Request& Req,
                      struct httplib::Response& Res);
    void HandleSession(const struct httplib::Request& Req,
                       struct httplib::Response& Res);
    /** @brief 返回编译进程序的赞赏码 JPEG；所有有效登录用户均可读取。 */
    void HandleDonationImage(const struct httplib::Request& Req,
                             struct httplib::Response& Res);
    void HandleRegisterInvite(const struct httplib::Request& Req,
                              struct httplib::Response& Res);
    void HandleRegisterInviteJoin(const struct httplib::Request& Req,
                                  struct httplib::Response& Res);
    void HandleRegister(const struct httplib::Request& Req,
                        struct httplib::Response& Res);

    // ---- 认证中间件 ----
    bool CheckAuth(const struct httplib::Request& Req,
                   struct httplib::Response& Res,
                   const std::string& ExtraToken = "");

    // ---- 挑战码管理 ----
    std::string GenerateChallenge();
    void        CleanupExpiredChallenges();

    // ---- 工具 ----
    static std::string EscapeJsonString(const std::string& S);
    static std::string BuildWSResponse(const std::string& Cmd,
                                       const std::string& DataJson);
    static std::string BuildWSError(const std::string& Cmd,
                                    const std::string& Message);
    static std::string HexEncode(const std::string& Data);
    static bool        IsStaticPathSafe(const std::string& Path);
    static std::string GetFileMimeType(const std::string& Path);

    // ---- JSON 辅助 ----
    static std::string JsonGetString(const std::string& Json,
                                     const std::string& Key);
    static int         JsonGetInt(const std::string& Json,
                                   const std::string& Key,
                                   int Default = 0);

    // ---- 数据成员 ----
    /** 编译进可执行程序的赞赏码 JPEG 原始字节，只保留一份静态数据。 */
    static const unsigned char s_DonationImageData[];
    /** 赞赏码 JPEG 的字节长度。 */
    static const std::size_t s_DonationImageSize;

    FWebServerConfig m_Config;
    std::unique_ptr<httplib::Server> m_Server;
    std::thread m_HttpThread;
    std::thread m_WsThread;
    std::atomic<bool> m_bRunning{false};
    std::atomic<bool> m_bStopRequested{false};
    std::atomic<bool> m_bWsListening{false};   // WS 监听线程是否成功启动

    // ---- 认证数据 ----
    std::map<std::string, FAuthUser> m_AuthUsers;  // Username → 用户配置
    bool        m_bAuthEnabled = false;            // 是否启用认证（数据库中存在有效用户）

    mutable std::mutex m_SessionMutex;               // 保护 m_Sessions 的读写
    std::map<std::string, FAuthSession> m_Sessions;  // Token → Session 映射

    // ---- 挑战码数据 ----
    mutable std::mutex m_ChallengeMutex;                            // 保护 m_LoginChallenges
    std::map<std::string, FLoginChallenge> m_LoginChallenges;      // Challenge → FLoginChallenge

    // 当前同步请求的认证上下文。thread_local 保证 httplib 并发工作线程之间身份严格隔离。
    static thread_local FAuthRequestContext s_RequestAuthContext;

    RuntimeSettingManager m_RuntimeSettings;
};

