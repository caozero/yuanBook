// WebServer.cpp — Web 服务实现
//
// HTTP API (基于 cpp-httplib)
// HTTP POST /api/channel → 数据通道 (复用 WebSocket 消息协议)
// WebSocket /ws → 可选数据通道 (基于 raw socket, RFC 6455, 默认关闭)
//
// 协议: {"cmd":"...","data":{...}}

#include "WebServer.h"
#include "AuthSecurity.h"
#include "JsonLite.h"
#include "WorkLog.h"
#include "SystemUtils.h"

#ifndef _WIN32
    #include <sys/time.h>
    #include <netdb.h>
#endif

// httplib.h 必须在 Windows 头文件之后包含 (WebServer.h 已处理)，POSIX 下需先提供 timeval/addrinfo 声明
#include "httplib.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <vector>
#include <random>
#include <mutex>
#include <iomanip>
#include <cctype>
#include <cstdint>

thread_local FAuthRequestContext FWebServer::s_RequestAuthContext{};

const unsigned char FWebServer::s_DonationImageData[] = {
#include "MoneyImage.inc"
};

const std::size_t FWebServer::s_DonationImageSize = sizeof(FWebServer::s_DonationImageData);

// ---- 跨平台 socket ----
#ifdef _WIN32
    #pragma comment(lib, "ws2_32.lib")
    #define SOCKET_TYPE     SOCKET
    #define INVALID_SOCK    INVALID_SOCKET
    #define SOCK_ERR        SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define SOCKET_TYPE     int
    #define INVALID_SOCK    (-1)
    #define SOCK_ERR        (-1)
    #define closesocket     close
#endif

namespace fs = std::filesystem;

// ============================================================================
// 协议辅助工具
// ============================================================================

namespace {

// Base64 编码 (仅用于 WebSocket 握手响应)
std::string Base64Encode(const std::string& data) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(kTable[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(kTable[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

} // anonymous namespace

// ============================================================================
// 极简 JSON 解析/构建
// ============================================================================

std::string FWebServer::JsonGetString(const std::string& Json, const std::string& Key)
{
    return JsonLite::GetStringOrDefault(Json, Key);
}

int FWebServer::JsonGetInt(const std::string& Json, const std::string& Key, int Default)
{
    return JsonLite::GetIntOrDefault(Json, Key, Default);
}

// 严格读取 JSON 字符串字段；字段值不是字符串时返回 false。
static bool JsonGetStringValueStrict(const std::string& Json, const std::string& Key,
                                     std::string& OutValue)
{
    return JsonLite::TryGetString(Json, Key, OutValue);
}

static bool JsonGetBoolValue(const std::string& Json, const std::string& Key, bool Default)
{
    return JsonLite::GetBoolOrDefault(Json, Key, Default);
}

static double JsonGetDoubleValue(const std::string& Json, const std::string& Key, double Default)
{
    return JsonLite::GetDoubleOrDefault(Json, Key, Default);
}

// 解析 JSON 数组的原始内容 (返回 "[" 到 "]" 之间的字符串, 不含外层括号)
static std::string JsonGetArrayRaw(const std::string& Json, const std::string& Key)
{
    return JsonLite::GetArrayRaw(Json, Key);
}

// 从 JSON 数组中提取所有顶层对象的原始字符串
static std::vector<std::string> JsonSplitArrayObjects(const std::string& ArrayRaw)
{
    return JsonLite::SplitTopLevelObjects(ArrayRaw);
}

// ============================================================================
// JSON 字符串转义与响应构造
// ============================================================================

std::string FWebServer::EscapeJsonString(const std::string& S)
{
    return JsonLite::EscapeString(S);
}

std::string FWebServer::BuildWSResponse(const std::string& Cmd, const std::string& DataJson)
{
    std::ostringstream oss;
    oss << "{\"cmd\":\"" << Cmd << "\",\"data\":" << DataJson << "}";
    return oss.str();
}

std::string FWebServer::BuildWSError(const std::string& Cmd, const std::string& Message)
{
    std::string errorCode = "REQUEST_FAILED";
    if (Message == "Authentication required") {
        errorCode = "AUTH_REQUIRED";
    } else if (Message.find("invalid or expired token") != std::string::npos) {
        errorCode = "AUTH_TOKEN_INVALID";
    } else if (Message.find("required permission") != std::string::npos) {
        errorCode = "PERMISSION_DENIED";
    } else if (Message.find("not a member of groupId") != std::string::npos) {
        errorCode = "NOT_GROUP_MEMBER";
    }

    std::ostringstream oss;
    oss << "{\"cmd\":\"" << EscapeJsonString(Cmd)
        << "\",\"data\":{\"error\":\"" << EscapeJsonString(Message)
        << "\",\"errorCode\":\"" << errorCode << "\"}}";
    return oss.str();
}

std::string FWebServer::HexEncode(const std::string& Data)
{
    std::ostringstream oss;
    for (unsigned char c : Data)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return oss.str();
}

bool FWebServer::IsStaticPathSafe(const std::string& Path)
{
    if (Path.empty()) return true;

    // 统一拒绝绝对路径、Windows 盘符、UNC 路径与路径穿越。
    if (Path[0] == '/' || Path[0] == '\\') return false;
    if (Path.size() >= 2 && Path[1] == ':') return false;
    if (Path.find("..") != std::string::npos) return false;
    if (Path.find('\\') != std::string::npos) return false;
    if (Path.find('\0') != std::string::npos) return false;

    return true;
}

std::string FWebServer::GetFileMimeType(const std::string& Path)
{
    fs::path p(Path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    static const std::map<std::string, std::string> kMimeMap = {
        {".html", "text/html; charset=utf-8"},
        {".htm",  "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        {".js",   "application/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".txt",  "text/plain; charset=utf-8"},
        {".svg",  "image/svg+xml"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".ico",  "image/x-icon"},
        {".webp", "image/webp"},
        {".woff", "font/woff"},
        {".woff2","font/woff2"},
        {".ttf",  "font/ttf"},
        {".eot",  "application/vnd.ms-fontobject"}
    };

    auto it = kMimeMap.find(ext);
    if (it != kMimeMap.end())
        return it->second;

    return "application/octet-stream";
}

// ============================================================================
// 认证仅从数据库加载；config.json 迁移与回退逻辑已移除
// ============================================================================

// ============================================================================
// 认证: 会话持久化 (SQLite auth_sessions)
// 注: 不再使用 sessions.json；登录信息永久有效直至主动登出
// ============================================================================

bool FWebServer::LoadSessionsFromDatabase()
{
    std::vector<FLedgerAuthSession> dbSessions;
    if (!m_LedgerManager.LoadAuthSessions(dbSessions)) {
        fprintf(stderr, "[WebServer] Auth: failed to load sessions from SQLite\n");
        return false;
    }

    std::vector<std::string> invalidTokens;
    int restoredCount = 0;
    int skippedCount = 0;

    {
        std::lock_guard<std::mutex> lock(m_SessionMutex);
        m_Sessions.clear();

        for (const auto& dbSession : dbSessions) {
            if (dbSession.Token.empty() || dbSession.Username.empty()) {
                skippedCount++;
                if (!dbSession.Token.empty()) invalidTokens.push_back(dbSession.Token);
                continue;
            }

            if (m_AuthUsers.find(dbSession.Username) == m_AuthUsers.end()) {
                skippedCount++;
                invalidTokens.push_back(dbSession.Token);
                std::string tokenTail = dbSession.Token.size() > 8 ? dbSession.Token.substr(dbSession.Token.size() - 8) : dbSession.Token;
                printf("[WebServer] Auth: skipped DB session for removed or unloaded user '%s', token=...%s\n",
                       dbSession.Username.c_str(), tokenTail.c_str());
                continue;
            }

            FAuthSession session;
            session.Token     = dbSession.Token;
            session.Username  = dbSession.Username;
            session.CreatedAt = dbSession.CreatedAt > 0 ? dbSession.CreatedAt : static_cast<int64_t>(std::time(nullptr));
            m_Sessions[session.Token] = session;
            restoredCount++;
        }

        if (m_Config.MaxSessionsPerUser > 0) {
            for (const auto& userPair : m_AuthUsers) {
                PruneOldSessionsForUserLocked(userPair.first);
            }
        }
    }

    if (!invalidTokens.empty()) {
        DeleteSessionsFromDatabase(invalidTokens);
    }

    printf("[WebServer] Auth: loaded %d active DB session(s), skipped %d invalid session(s), per-user limit=%d\n",
           restoredCount, skippedCount, m_Config.MaxSessionsPerUser);
    return true;
}

bool FWebServer::SaveSessionToDatabase(const FAuthSession& Session)
{
    if (Session.Token.empty() || Session.Username.empty()) return false;
    return m_LedgerManager.UpsertAuthSession(Session.Token, Session.Username, Session.CreatedAt);
}

bool FWebServer::DeleteSessionsFromDatabase(const std::vector<std::string>& Tokens)
{
    return m_LedgerManager.DeleteAuthSessionsByTokens(Tokens);
}

int FWebServer::RemoveUserSessionsFromMemory(const std::string& Username)
{
    if (Username.empty()) return 0;

    std::lock_guard<std::mutex> lock(m_SessionMutex);
    int removedCount = 0;
    for (auto it = m_Sessions.begin(); it != m_Sessions.end(); ) {
        if (it->second.Username == Username) {
            it = m_Sessions.erase(it);
            ++removedCount;
        } else {
            ++it;
        }
    }
    return removedCount;
}

int FWebServer::PruneOldSessionsForUserLocked(const std::string& Username,
                                              const std::string& KeepToken)
{
    // 调用者必须已持有 m_SessionMutex。
    if (m_Config.MaxSessionsPerUser <= 0) return 0;

    std::vector<std::pair<int64_t, std::string>> userSessions;
    userSessions.reserve(m_Sessions.size());

    for (const auto& pair : m_Sessions) {
        if (pair.second.Username == Username) {
            userSessions.emplace_back(pair.second.CreatedAt, pair.first);
        }
    }

    const size_t maxKeep = static_cast<size_t>(m_Config.MaxSessionsPerUser);
    if (userSessions.size() <= maxKeep) return 0;

    std::sort(userSessions.begin(), userSessions.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });

    int removedCount = 0;
    std::vector<std::string> removedTokens;
    for (const auto& item : userSessions) {
        if (m_Sessions.size() == 0) break;

        size_t remainingForUser = 0;
        for (const auto& pair : m_Sessions) {
            if (pair.second.Username == Username) remainingForUser++;
        }
        if (remainingForUser <= maxKeep) break;

        const std::string& token = item.second;
        if (!KeepToken.empty() && token == KeepToken) continue;

        if (m_Sessions.erase(token) > 0) {
            removedTokens.push_back(token);
            removedCount++;
        }
    }

    if (!removedTokens.empty() && !DeleteSessionsFromDatabase(removedTokens)) {
        fprintf(stderr, "[WebServer] Auth: failed to delete pruned DB session(s) for user '%s'\n",
                Username.c_str());
    }

    if (removedCount > 0) {
        printf("[WebServer] Auth: pruned %d old DB session(s) for user '%s' (limit=%d)\n",
               removedCount, Username.c_str(), m_Config.MaxSessionsPerUser);
    }
    return removedCount;
}

// ============================================================================
// 认证: Token 生成 (48 字符十六进制, 共用已引入的 <random>)
// ============================================================================

std::string FWebServer::GenerateToken() const
{
    static thread_local std::mt19937_64 rng(
        static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
            reinterpret_cast<uintptr_t>(&rng)
        )
    );
    static thread_local std::uniform_int_distribution<uint64_t> dist;

    uint64_t parts[3];
    for (int i = 0; i < 3; ++i)
        parts[i] = dist(rng);

    std::ostringstream oss;
    for (int i = 0; i < 3; ++i)
        oss << std::hex << std::setw(16) << std::setfill('0') << parts[i];
    return oss.str();
}

// ============================================================================
// 认证: 密码哈希 (SHA1, 复用已有实现)
// ============================================================================

std::string FWebServer::HashPassword(const std::string& Password) const
{
    return AuthSecurity::HashPasswordForStorage(Password);
}

// ============================================================================
// 挑战码管理: 生成随机 challenge（48位十六进制字符串）
// ============================================================================

std::string FWebServer::GenerateChallenge()
{
    static thread_local std::mt19937_64 rng(
        static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
            reinterpret_cast<uintptr_t>(&rng)
        )
    );
    static thread_local std::uniform_int_distribution<uint64_t> dist;

    uint64_t parts[3];
    for (int i = 0; i < 3; ++i)
        parts[i] = dist(rng);

    std::ostringstream oss;
    for (int i = 0; i < 3; ++i)
        oss << std::hex << std::setw(16) << std::setfill('0') << parts[i];
    return oss.str();
}

// ============================================================================
// 挑战码管理: 清理过期 challenge（超过 60 秒）
// ============================================================================

void FWebServer::CleanupExpiredChallenges()
{
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t kChallengeTTL = 60; // 60 秒过期

    std::lock_guard<std::mutex> lock(m_ChallengeMutex);
    for (auto it = m_LoginChallenges.begin(); it != m_LoginChallenges.end(); ) {
        if (now - it->second.CreatedAt > kChallengeTTL)
            it = m_LoginChallenges.erase(it);
        else
            ++it;
    }
}

// ============================================================================
// HTTP 认证接口: GET /api/login/precheck?username=xxx
// 账号输入框失焦时预检：仅当用户存在且 password_hash 为空时返回 needsPasswordSetup=true。
// ============================================================================

void FWebServer::HandleLoginPrecheck(const httplib::Request& Req, httplib::Response& Res)
{
    std::string username;
    if (Req.has_param("username"))
        username = Req.get_param_value("username");

    if (username.empty() || !m_bAuthEnabled) {
        Res.set_content("{\"ok\":true,\"exists\":false,\"needsPasswordSetup\":false}",
                        "application/json; charset=utf-8");
        return;
    }

    std::string storedHash;
    bool exists = m_LedgerManager.GetUserPasswordHash(username, storedHash);
    bool needsPasswordSetup = exists && storedHash.empty();

    std::ostringstream oss;
    oss << "{\"ok\":true,\"exists\":" << (exists ? "true" : "false")
        << ",\"needsPasswordSetup\":" << (needsPasswordSetup ? "true" : "false")
        << "}";
    Res.set_content(oss.str(), "application/json; charset=utf-8");
}

// ============================================================================
// HTTP 认证接口: GET /api/login/challenge?username=xxx
// 生成一次性挑战码返回给前端，用于 challenge-response 登录
// 响应: {ok:true, challenge:"48位十六进制字符串"}
// ============================================================================

void FWebServer::HandleLoginChallenge(const httplib::Request& Req, httplib::Response& Res)
{
    if (!m_bAuthEnabled) {
        Res.status = 200;
        Res.set_content("{\"ok\":false,\"message\":\"auth disabled\"}", "application/json");
        return;
    }

    std::string username;
    if (Req.has_param("username"))
        username = Req.get_param_value("username");

    if (username.empty()) {
        Res.status = 400;
        Res.set_content("{\"ok\":false,\"message\":\"Missing username\"}",
                        "application/json; charset=utf-8");
        return;
    }

    // 检查用户是否存在
    bool userExists = false;
    {
        auto userIt = m_AuthUsers.find(username);
        if (userIt != m_AuthUsers.end())
            userExists = true;
    }
    if (!userExists) {
        // 也检查数据库。注意不能调用 AuthenticateUser()，该函数会对空密码用户执行首次初始化。
        std::string tempHash;
        if (m_LedgerManager.GetUserPasswordHash(username, tempHash))
            userExists = true;
    }
    if (!userExists) {
        // 不暴露用户是否存在，统一返回 challenge（但前端无用）
        // 实际也可返回 404，但这会暴露用户名有效性
    }

    // 先清理过期 challenge
    CleanupExpiredChallenges();

    // 生成新 challenge
    std::string challenge = GenerateChallenge();
    FLoginChallenge ch;
    ch.Challenge = challenge;
    ch.Username = username;
    ch.CreatedAt = static_cast<int64_t>(std::time(nullptr));

    {
        std::lock_guard<std::mutex> lock(m_ChallengeMutex);
        m_LoginChallenges[challenge] = ch;
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"challenge\":\"" << challenge << "\"}";
    Res.set_content(oss.str(), "application/json; charset=utf-8");
}

// ============================================================================
// 认证: 公开接口 — 校验 token 有效性
// ============================================================================

bool FWebServer::IsTokenValid(const std::string& Token) const
{
    if (!m_bAuthEnabled) return true;  // 未启用认证则放行
    if (Token.empty()) return false;

    std::lock_guard<std::mutex> lock(m_SessionMutex);

    auto it = m_Sessions.find(Token);
    if (it == m_Sessions.end())
        return false;

    return true;
}

// ============================================================================
// 认证: 公开接口 — 获取用户权限 (JSON 数组字符串, 如 "[\"admin\"]")
// ============================================================================

std::string FWebServer::GetUserPermissions(const std::string& Username) const
{
    auto it = m_AuthUsers.find(Username);
    if (it == m_AuthUsers.end())
        return "[\"user\"]";

    std::string permissions = it->second.Permissions.empty() ? "[\"user\"]" : it->second.Permissions;
    if (permissions.find("\"user\"") == std::string::npos) {
        if (permissions == "[]") {
            permissions = "[\"user\"]";
        } else {
            permissions.insert(permissions.size() - 1, ",\"user\"");
        }
    }
    return permissions;
}

// ============================================================================
// 认证: 中间件 — 检查 HTTP 请求是否携带有效 token
// Token 来源优先级: Header > Query > Body JSON
// ============================================================================

bool FWebServer::CheckAuth(const httplib::Request& Req,
                            httplib::Response& Res,
                            const std::string& ExtraToken)
{
    if (!m_bAuthEnabled) return true;

    std::string token;

    // 1. 尝试从 ExtraToken 获取 (如 WebSocket 中的 token)
    if (!ExtraToken.empty()) {
        token = ExtraToken;
    }

    // 2. 尝试从 Authorization header 获取 (Bearer <token>)
    if (token.empty() && Req.has_header("Authorization")) {
        std::string authHeader = Req.get_header_value("Authorization");
        const char* prefix = "Bearer ";
        if (authHeader.size() >= strlen(prefix) &&
            authHeader.compare(0, strlen(prefix), prefix) == 0) {
            token = authHeader.substr(strlen(prefix));
        }
    }

    // 3. 尝试从 query 参数获取
    if (token.empty() && Req.has_param("token")) {
        token = Req.get_param_value("token");
    }

    // 4. 尝试从请求体 JSON 获取
    if (token.empty() && !Req.body.empty()) {
        token = JsonGetString(Req.body, "token");
    }

    if (IsTokenValid(token))
        return true;

    // 认证失败
    Res.status = 401;
    Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Invalid or missing token\"}",
                    "application/json");
    return false;
}

// ============================================================================
// HTTP 认证接口: POST /api/login
// 支持两种请求格式:
//   旧版: {"username":"...","password":"..."}                       — 明文密码（向后兼容）
//   新版: {"username":"...","challenge":"...","response":"..."}     — Challenge-Response
// 响应: {"ok":true,"token":"...","username":"...","permissions":[...]}
// 注: 多用户支持, 超过每用户会话上限时清理最旧会话, 不返回过期时间
// ============================================================================

void FWebServer::HandleLogin(const httplib::Request& Req, httplib::Response& Res)
{
    if (!m_bAuthEnabled) {
        Res.status = 200;
        Res.set_content("{\"ok\":true,\"message\":\"auth disabled\"}", "application/json");
        return;
    }

    std::string username = JsonGetString(Req.body, "username");
    std::string challenge = JsonGetString(Req.body, "challenge");
    std::string response  = JsonGetString(Req.body, "response");
    std::string password  = JsonGetString(Req.body, "password");
    std::string passwordHash = JsonGetString(Req.body, "passwordHash");
    bool passwordSetup = JsonGetBoolValue(Req.body, "passwordSetup", false);
    bool passwordInitialized = false;

    if (username.empty()) {
        Res.status = 400;
        Res.set_content("{\"error\":\"bad_request\",\"message\":\"Missing username\"}",
                        "application/json");
        return;
    }

    std::string inputHash;
    std::string userPermissions = "[]";
    bool bFound = false;

    if (!challenge.empty() && !response.empty()) {
        // ---- Challenge-Response 认证模式 ----
        // 前端计算: response = SHA1_Hex(challenge + SHA1_Hex(password))
        // 服务端验证: response == SHA1_Hex(challenge + storedHash)

        // 1. 验证 challenge 是否存在且未过期
        FLoginChallenge ch;
        bool challengeValid = false;
        {
            std::lock_guard<std::mutex> lock(m_ChallengeMutex);
            auto it = m_LoginChallenges.find(challenge);
            if (it != m_LoginChallenges.end()) {
                int64_t now = static_cast<int64_t>(std::time(nullptr));
                if (now - it->second.CreatedAt <= 60 && it->second.Username == username) {
                    ch = it->second;
                    challengeValid = true;
                }
                // 无论是否过期，使用后立即删除（防重放）
                m_LoginChallenges.erase(it);
            }
        }

        if (!challengeValid) {
            printf("[WebServer][LoginDebug] challenge invalid: username='%s', challenge='%s'\n",
                   username.c_str(), challenge.c_str());
            Res.status = 401;
            Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Challenge expired or invalid\"}",
                            "application/json");
            return;
        }

        // 2. 获取用户的密码哈希
        std::string storedHash;
        {
            auto userIt = m_AuthUsers.find(username);
            if (userIt != m_AuthUsers.end() && !userIt->second.PasswordHash.empty()) {
                storedHash = userIt->second.PasswordHash;
            }
        }

        // 内存中默认不缓存数据库密码哈希，challenge-response 必须从数据库读取。
        if (storedHash.empty()) {
            m_LedgerManager.GetUserPasswordHash(username, storedHash);
        }

        if (storedHash.empty()) {
            if (!passwordSetup || passwordHash.empty()) {
                printf("[WebServer][LoginDebug] storedHash empty without setup: username='%s', challenge='%s', response='%s'\n",
                       username.c_str(), challenge.c_str(), response.c_str());
                Res.status = 401;
                Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Invalid username or password\"}",
                                "application/json");
                return;
            }

            std::string setupExpected = AuthSecurity::Sha1Hex(challenge + passwordHash);
            if (response != setupExpected) {
                printf("[WebServer][LoginDebug] password setup response mismatch for user '%s'\n", username.c_str());
                Res.status = 401;
                Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Invalid username or password\"}",
                                "application/json");
                return;
            }

            // 二次确认数据库仍为空后才写入，避免前端状态被伪造时覆盖已有密码。
            std::string currentHash;
            if (!m_LedgerManager.GetUserPasswordHash(username, currentHash) || !currentHash.empty()) {
                Res.status = 409;
                Res.set_content("{\"error\":\"conflict\",\"message\":\"Password has already been initialized\"}",
                                "application/json");
                return;
            }

            if (!m_LedgerManager.UpdateUserPassword(username, passwordHash)) {
                Res.status = 500;
                Res.set_content("{\"error\":\"server_error\",\"message\":\"Failed to initialize password\"}",
                                "application/json");
                return;
            }

            storedHash = passwordHash;
            passwordInitialized = true;
            {
                auto userIt = m_AuthUsers.find(username);
                if (userIt != m_AuthUsers.end()) {
                    userIt->second.PasswordHash = storedHash;
                }
            }
            printf("[WebServer] Auth: initialized password for user '%s' via challenge-response setup\n",
                   username.c_str());
        }

        // 3. 计算期望值并比对
        std::string challengeAndHash = challenge + storedHash;
        std::string expected = AuthSecurity::Sha1Hex(challengeAndHash);
        printf("[WebServer][LoginDebug] username='%s', challenge='%s', storedHash='%s', challenge+storedHash='%s', clientResponse='%s', expected='%s'\n",
               username.c_str(),
               challenge.c_str(),
               storedHash.c_str(),
               challengeAndHash.c_str(),
               response.c_str(),
               expected.c_str());
        if (response != expected) {
            printf("[WebServer][LoginDebug] response mismatch for user '%s'\n", username.c_str());
            Res.status = 401;
            Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Invalid username or password\"}",
                            "application/json");
            return;
        }

        // 4. 获取用户权限
        bFound = true;
        {
            auto userIt = m_AuthUsers.find(username);
            if (userIt != m_AuthUsers.end()) {
                userPermissions = userIt->second.Permissions;
            }
        }
        // 尝试从数据库获取权限（如果内存中不存在）
        if (userPermissions == "[]") {
            m_LedgerManager.AuthenticateUser(username, storedHash, userPermissions);
        }

        printf("[WebServer] Auth: user '%s' authenticated via challenge-response\n",
               username.c_str());
    } else {
        // ---- 旧版兼容: 明文密码认证 ----
        if (password.empty()) {
            Res.status = 400;
            Res.set_content("{\"error\":\"bad_request\",\"message\":\"Missing password or challenge/response\"}",
                            "application/json");
            return;
        }

        inputHash = AuthSecurity::HashPasswordForStorage(password);

        // 优先从数据库认证
        if (m_LedgerManager.AuthenticateUser(username, inputHash, userPermissions)) {
            bFound = true;
            printf("[WebServer] Auth: user '%s' authenticated via LedgerManager\n",
                   username.c_str());
        }

        // 回退：使用内存中的旧版认证镜像（仅数据库失败/兼容路径）
        if (!bFound) {
            auto userIt = m_AuthUsers.find(username);
            if (userIt != m_AuthUsers.end()) {
                // 若用户 passwordHash 为空, 表示首次登录或重置密码: 用本次密码初始化并写回数据库
                if (userIt->second.PasswordHash.empty()) {
                    if (m_LedgerManager.UpdateUserPassword(username, inputHash)) {
                        userIt->second.PasswordHash = inputHash;
                        printf("[WebServer] Auth: user '%s' password initialized on first login\n",
                               username.c_str());
                        bFound = true;
                        userPermissions = userIt->second.Permissions;
                    } else {
                        fprintf(stderr, "[WebServer] Auth: failed to initialize password for user '%s'\n",
                                username.c_str());
                    }
                } else if (inputHash == userIt->second.PasswordHash) {
                    bFound = true;
                    userPermissions = userIt->second.Permissions;
                }
            }
        }
    }

    if (!bFound) {
        Res.status = 401;
        Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Invalid username or password\"}",
                        "application/json");
        return;
    }

    // 生成会话；若超过每用户会话上限，清理该用户最旧的会话。
    std::string token = GenerateToken();
    FAuthSession session;
    session.Token     = token;
    session.Username  = username;
    session.CreatedAt = static_cast<int64_t>(std::time(nullptr));

    if (!SaveSessionToDatabase(session)) {
        Res.status = 500;
        Res.set_content("{\"error\":\"server_error\",\"message\":\"Failed to save session\"}",
                        "application/json");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_SessionMutex);
        m_Sessions[token] = session;
        PruneOldSessionsForUserLocked(username, token);
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"token\":\"" << token << "\","
        << "\"username\":\"" << EscapeJsonString(username) << "\","
        << "\"permissions\":" << userPermissions << ","
        << "\"passwordInitialized\":" << (passwordInitialized ? "true" : "false") << "}";
    Res.set_content(oss.str(), "application/json");

    printf("[WebServer] Auth: user '%s' logged in, token=...%s\n",
           username.c_str(), token.substr(token.size() - 8).c_str());
}

// ============================================================================
// HTTP 邀请注册接口: GET /api/register_invite?code=xxxx
// ============================================================================

void FWebServer::HandleRegisterInvite(const httplib::Request& Req, httplib::Response& Res)
{
    std::string code;
    if (Req.has_param("code")) code = Req.get_param_value("code");
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    FLedgerInviteRegistration invite;
    std::string err;
    if (!m_LedgerManager.GetValidInviteRegistration(code, invite, err)) {
        Res.status = 404;
        Res.set_content("{\"ok\":false,\"message\":\"需要邀请注册，或者邀请链接已经过期\"}",
                        "application/json; charset=utf-8");
        return;
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,"
        << "\"code\":\"" << EscapeJsonString(invite.Code) << "\","
        << "\"autoJoinFamily\":" << (invite.bAutoJoinFamily ? "true" : "false") << ","
        << "\"familyId\":" << invite.FamilyId << ","
        << "\"defaultLedgerId\":" << invite.DefaultLedgerId << ","
        << "\"familyRole\":\"" << EscapeJsonString(invite.FamilyRole) << "\","
        << "\"expiresAt\":" << invite.ExpiresAt
        << "}";
    Res.set_content(oss.str(), "application/json; charset=utf-8");
}

// ============================================================================
// HTTP 邀请加入接口: POST /api/register_invite/join
// 已登录用户使用有效邀请链接加入目标账本。
// ============================================================================

void FWebServer::HandleRegisterInviteJoin(const httplib::Request& Req, httplib::Response& Res)
{
    if (!m_bAuthEnabled) {
        Res.status = 401;
        Res.set_content("{\"ok\":false,\"message\":\"Authentication required\"}",
                        "application/json; charset=utf-8");
        return;
    }

    std::string token;
    if (Req.has_header("Authorization")) {
        std::string authHeader = Req.get_header_value("Authorization");
        const char* prefix = "Bearer ";
        if (authHeader.size() >= strlen(prefix) &&
            authHeader.compare(0, strlen(prefix), prefix) == 0) {
            token = authHeader.substr(strlen(prefix));
        }
    }
    if (token.empty() && Req.has_param("token")) {
        token = Req.get_param_value("token");
    }
    if (token.empty() && !Req.body.empty()) {
        token = JsonGetString(Req.body, "token");
    }

    std::string username;
    {
        std::lock_guard<std::mutex> lock(m_SessionMutex);
        auto it = m_Sessions.find(token);
        if (it != m_Sessions.end()) {
            username = it->second.Username;
        }
    }
    if (username.empty()) {
        Res.status = 401;
        Res.set_content("{\"ok\":false,\"message\":\"Invalid or missing token\"}",
                        "application/json; charset=utf-8");
        return;
    }

    std::string code = JsonGetString(Req.body, "code");
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (code.empty()) {
        Res.status = 400;
        Res.set_content("{\"ok\":false,\"message\":\"Missing invite code\"}",
                        "application/json; charset=utf-8");
        return;
    }

    FLedgerInviteRegistration invite;
    std::string err;
    if (!m_LedgerManager.ApplyInviteCodeToExistingUser(code, username, invite, err)) {
        Res.status = 403;
        std::ostringstream error;
        error << "{\"ok\":false,\"message\":\""
              << EscapeJsonString(err.empty() ? "邀请加入失败" : err) << "\"}";
        Res.set_content(error.str(), "application/json; charset=utf-8");
        return;
    }

    std::string permissions = GetUserPermissions(username);
    std::ostringstream oss;
    oss << "{\"ok\":true,\"token\":\"" << EscapeJsonString(token) << "\","
        << "\"username\":\"" << EscapeJsonString(username) << "\","
        << "\"permissions\":" << permissions << ","
        << "\"autoJoinFamily\":" << (invite.bAutoJoinFamily ? "true" : "false") << ","
        << "\"familyId\":" << invite.FamilyId << ","
        << "\"defaultLedgerId\":" << invite.DefaultLedgerId << ","
        << "\"familyRole\":\"" << EscapeJsonString(invite.FamilyRole) << "\""
        << "}";
    Res.set_content(oss.str(), "application/json; charset=utf-8");
}

// ============================================================================
// HTTP 邀请注册接口: POST /api/register
// ============================================================================

void FWebServer::HandleRegister(const httplib::Request& Req, httplib::Response& Res)
{
    std::string code = JsonGetString(Req.body, "code");
    std::string username = JsonGetString(Req.body, "username");
    std::string challenge = JsonGetString(Req.body, "challenge");
    std::string response  = JsonGetString(Req.body, "response");
    std::string passwordHash = JsonGetString(Req.body, "passwordHash");
    std::string password  = JsonGetString(Req.body, "password");
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    if (code.empty() || username.empty()) {
        Res.status = 400;
        Res.set_content("{\"ok\":false,\"message\":\"Missing code or username\"}",
                        "application/json; charset=utf-8");
        return;
    }

    FLedgerInviteRegistration invite;
    std::string err;
    if (!m_LedgerManager.GetValidInviteRegistration(code, invite, err)) {
        Res.status = 403;
        Res.set_content("{\"ok\":false,\"message\":\"需要邀请注册，或者邀请链接已经过期\"}",
                        "application/json; charset=utf-8");
        return;
    }

    // ---- Challenge-Response 注册模式 ----
    if (!challenge.empty() && !response.empty() && !passwordHash.empty()) {
        // 验证 challenge 有效性
        bool challengeValid = false;
        {
            std::lock_guard<std::mutex> lock(m_ChallengeMutex);
            auto it = m_LoginChallenges.find(challenge);
            if (it != m_LoginChallenges.end()) {
                int64_t now = static_cast<int64_t>(std::time(nullptr));
                if (now - it->second.CreatedAt <= 60 && it->second.Username == username) {
                    challengeValid = true;
                }
                m_LoginChallenges.erase(it);
            }
        }

        if (!challengeValid) {
            Res.status = 401;
            Res.set_content("{\"ok\":false,\"message\":\"Challenge expired or invalid\"}",
                            "application/json; charset=utf-8");
            return;
        }

        // 验证 response: response == SHA1_Hex(challenge + passwordHash)
        std::string expected = AuthSecurity::Sha1Hex(challenge + passwordHash);
        if (response != expected) {
            Res.status = 401;
            Res.set_content("{\"ok\":false,\"message\":\"校验失败\"}",
                            "application/json; charset=utf-8");
            return;
        }
    } else {
        // ---- 旧版兼容: 明文密码注册 ----
        if (password.empty()) {
            Res.status = 400;
            Res.set_content("{\"ok\":false,\"message\":\"Missing password or challenge/response\"}",
                            "application/json; charset=utf-8");
            return;
        }
        passwordHash = AuthSecurity::HashPasswordForStorage(password);
    }

    // 用户创建、家庭加入与当前上下文设置必须由领域层在同一事务内完成。
    if (!m_LedgerManager.RegisterUserWithInvite(
            code,
            username,
            passwordHash,
            "[\"user\"]",
            invite,
            err)) {
        Res.status = err.find("用户名") != std::string::npos ? 409 : 403;
        std::ostringstream error;
        error << "{\"ok\":false,\"message\":\""
              << EscapeJsonString(err.empty() ? "注册失败" : err) << "\"}";
        Res.set_content(error.str(), "application/json; charset=utf-8");
        return;
    }

    FAuthUser user;
    user.Username = username;
    user.PasswordHash = passwordHash;
    user.Permissions = "[\"user\"]";
    m_AuthUsers[username] = user;
    m_bAuthEnabled = true;

    std::string token = GenerateToken();
    FAuthSession session;
    session.Token = token;
    session.Username = username;
    session.CreatedAt = static_cast<int64_t>(std::time(nullptr));

    if (!SaveSessionToDatabase(session)) {
        Res.status = 500;
        Res.set_content("{\"ok\":false,\"message\":\"Failed to save session\"}",
                        "application/json; charset=utf-8");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_SessionMutex);
        m_Sessions[token] = session;
        PruneOldSessionsForUserLocked(username, token);
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"token\":\"" << token << "\","
        << "\"username\":\"" << EscapeJsonString(username) << "\","
        << "\"permissions\":[\"user\"],"
        << "\"autoJoinFamily\":" << (invite.bAutoJoinFamily ? "true" : "false") << ","
        << "\"familyId\":" << invite.FamilyId << ","
        << "\"defaultLedgerId\":" << invite.DefaultLedgerId << ","
        << "\"familyRole\":\"" << EscapeJsonString(invite.FamilyRole) << "\""
        << "}";
    Res.set_content(oss.str(), "application/json; charset=utf-8");
}

// ============================================================================
// HTTP 认证接口: POST /api/logout
// 请求: {"token":"..."} 或 Authorization header
// 响应: {"ok":true}
// ============================================================================

void FWebServer::HandleLogout(const httplib::Request& Req, httplib::Response& Res)
{
    if (!m_bAuthEnabled) {
        Res.set_content("{\"ok\":true}", "application/json");
        return;
    }

    std::string token;

    if (Req.has_header("Authorization")) {
        std::string authHeader = Req.get_header_value("Authorization");
        const char* prefix = "Bearer ";
        if (authHeader.size() >= strlen(prefix) &&
            authHeader.compare(0, strlen(prefix), prefix) == 0) {
            token = authHeader.substr(strlen(prefix));
        }
    }

    if (token.empty())
        token = JsonGetString(Req.body, "token");

    if (!token.empty()) {
        std::string username;
        {
            std::lock_guard<std::mutex> lock(m_SessionMutex);
            auto it = m_Sessions.find(token);
            if (it != m_Sessions.end()) {
                username = it->second.Username;
                m_Sessions.erase(it);
            }
        }
        if (!username.empty()) {
            printf("[WebServer] Auth: user '%s' logged out\n", username.c_str());
            std::vector<std::string> tokensToDelete;
            tokensToDelete.push_back(token);
            if (!DeleteSessionsFromDatabase(tokensToDelete)) {
                fprintf(stderr, "[WebServer] Auth: failed to delete DB session for user '%s'\n",
                        username.c_str());
            }
        }
    }

    Res.set_content("{\"ok\":true}", "application/json");
}

// ============================================================================
// HTTP 认证接口: GET /api/session
// 检查 token 是否有效, 返回当前用户信息和权限
// ============================================================================

void FWebServer::HandleSession(const httplib::Request& Req, httplib::Response& Res)
{
    if (!m_bAuthEnabled) {
        Res.set_content("{\"ok\":true,\"authEnabled\":false}", "application/json");
        return;
    }

    std::string token;

    if (Req.has_header("Authorization")) {
        std::string authHeader = Req.get_header_value("Authorization");
        const char* prefix = "Bearer ";
        if (authHeader.size() >= strlen(prefix) &&
            authHeader.compare(0, strlen(prefix), prefix) == 0) {
            token = authHeader.substr(strlen(prefix));
        }
    }

    if (token.empty() && Req.has_param("token"))
        token = Req.get_param_value("token");

    if (token.empty()) {
        Res.status = 401;
        Res.set_content("{\"error\":\"unauthorized\",\"message\":\"No token provided\"}",
                        "application/json");
        return;
    }

    std::string username;
    {
        std::lock_guard<std::mutex> lock(m_SessionMutex);
        auto it = m_Sessions.find(token);
        if (it == m_Sessions.end()) {
            Res.status = 401;
            Res.set_content("{\"error\":\"unauthorized\",\"message\":\"Invalid token\"}",
                            "application/json");
            return;
        }
        username = it->second.Username;
    }

    std::string permissions = GetUserPermissions(username);

    std::ostringstream oss;
    oss << "{\"ok\":true,\"token\":\"" << token << "\","
        << "\"username\":\"" << EscapeJsonString(username) << "\","
        << "\"permissions\":" << permissions << "}";
    Res.set_content(oss.str(), "application/json");
}

void FWebServer::HandleDonationImage(const httplib::Request& Req, httplib::Response& Res)
{
    if (!CheckAuth(Req, Res)) {
        return;
    }

    if (s_DonationImageSize == 0) {
        Res.status = 500;
        Res.set_content("{\"error\":\"server_error\",\"message\":\"Donation image is unavailable\"}",
                        "application/json; charset=utf-8");
        return;
    }

    Res.set_header("Cache-Control", "private, max-age=3600");
    Res.set_header("X-Content-Type-Options", "nosniff");
    Res.set_content(reinterpret_cast<const char*>(s_DonationImageData),
                    s_DonationImageSize,
                    "image/jpeg");
}

// ============================================================================
// WebSocket 帧收发 (RFC 6455, Section 5)
// ============================================================================

// 发送文本帧
static bool WS_SendText(SOCKET_TYPE Sock, const std::string& Message)
{
    // Frame: FIN=1, Opcode=0x1 (text), Mask=0 (server→client)
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + Text opcode

    size_t len = Message.size();
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }

    frame.insert(frame.end(), Message.begin(), Message.end());

    int totalSent = 0;
    while (totalSent < static_cast<int>(frame.size())) {
        int sent = ::send(Sock, reinterpret_cast<const char*>(frame.data() + totalSent),
                          static_cast<int>(frame.size()) - totalSent, 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

// 接收一帧 (阻塞, 带超时)
static bool WS_RecvFrame(SOCKET_TYPE Sock, std::string& OutPayload, int TimeoutMs)
{
    // 设置接收超时
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(TimeoutMs);
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = TimeoutMs / 1000;
    tv.tv_usec = (TimeoutMs % 1000) * 1000;
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // 读取前 2 字节
    uint8_t header[2];
    int r = ::recv(Sock, reinterpret_cast<char*>(header), 2, MSG_WAITALL);
    if (r != 2) return false;

    bool fin    = (header[0] & 0x80) != 0;
    int  opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payloadLen = header[1] & 0x7F;

    (void)fin;
    (void)masked;

    // Opcode 检查: 支持 text(1), close(8), ping(9), pong(10)
    if (opcode == 0x8) return false;  // Close frame

    // 扩展长度
    if (payloadLen == 126) {
        uint8_t ext[2];
        r = ::recv(Sock, reinterpret_cast<char*>(ext), 2, MSG_WAITALL);
        if (r != 2) return false;
        payloadLen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        r = ::recv(Sock, reinterpret_cast<char*>(ext), 8, MSG_WAITALL);
        if (r != 8) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i)
            payloadLen = (payloadLen << 8) | ext[i];
    }

    // 读取 mask key (客户端→服务器 必定有 mask)
    uint8_t maskKey[4] = {0};
    if (masked) {
        r = ::recv(Sock, reinterpret_cast<char*>(maskKey), 4, MSG_WAITALL);
        if (r != 4) return false;
    }

    // 读取 payload
    if (payloadLen > 10 * 1024 * 1024) return false; // 最大 10MB

    std::vector<uint8_t> payload(static_cast<size_t>(payloadLen));
    size_t totalRecv = 0;
    while (totalRecv < payload.size()) {
        r = ::recv(Sock, reinterpret_cast<char*>(payload.data() + totalRecv),
                   static_cast<int>(payload.size() - totalRecv), 0);
        if (r <= 0) return false;
        totalRecv += static_cast<size_t>(r);
    }

    // 去 mask
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= maskKey[i % 4];
    }

    // 处理 control frames (Ping → Pong, 递归读取直到 text frame)
    if (opcode == 0x9) {
        // Ping → Pong: 发送 pong frame (opcode 0xA), echo payload
        std::vector<uint8_t> pongFrame;
        pongFrame.push_back(0x8A); // FIN + Pong opcode
        size_t pongLen = payload.size();
        if (pongLen <= 125) {
            pongFrame.push_back(static_cast<uint8_t>(pongLen));
        } else if (pongLen <= 0xFFFF) {
            pongFrame.push_back(126);
            pongFrame.push_back(static_cast<uint8_t>((pongLen >> 8) & 0xFF));
            pongFrame.push_back(static_cast<uint8_t>(pongLen & 0xFF));
        } else {
            pongFrame.push_back(127);
            for (int i = 7; i >= 0; --i)
                pongFrame.push_back(static_cast<uint8_t>((pongLen >> (i * 8)) & 0xFF));
        }
        pongFrame.insert(pongFrame.end(), payload.begin(), payload.end());
        int totalSent = 0;
        while (totalSent < static_cast<int>(pongFrame.size())) {
            int sent = ::send(Sock, reinterpret_cast<const char*>(pongFrame.data() + totalSent),
                              static_cast<int>(pongFrame.size()) - totalSent, 0);
            if (sent <= 0) return false;
            totalSent += sent;
        }
        return WS_RecvFrame(Sock, OutPayload, TimeoutMs);
    }
    if (opcode == 0xA) {
        // Pong: 忽略，读取下一帧
        return WS_RecvFrame(Sock, OutPayload, TimeoutMs);
    }

    OutPayload.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    return true;
}

// ============================================================================
// WebSocket 握手处理
// ============================================================================

static bool WS_PerformHandshake(SOCKET_TYPE Sock, int TimeoutMs)
{
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(TimeoutMs);
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = TimeoutMs / 1000;
    tv.tv_usec = (TimeoutMs % 1000) * 1000;
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // 读取 HTTP 升级请求
    char buf[8192];
    int totalRecv = 0;
    while (totalRecv < (int)sizeof(buf) - 1) {
        int r = ::recv(Sock, buf + totalRecv, 1, 0);
        if (r <= 0) return false;
        totalRecv += r;
        // 检测 \r\n\r\n (HTTP 头结束)
        if (totalRecv >= 4 &&
            buf[totalRecv-4] == '\r' && buf[totalRecv-3] == '\n' &&
            buf[totalRecv-2] == '\r' && buf[totalRecv-1] == '\n')
            break;
    }
    buf[totalRecv] = '\0';
    std::string request(buf, totalRecv);

    // 提取 Sec-WebSocket-Key
    std::string wsKey;
    const char* keyPhrase = "Sec-WebSocket-Key: ";
    const char* keyStart = strstr(buf, keyPhrase);
    if (keyStart) {
        keyStart += strlen(keyPhrase);
        const char* keyEnd = strstr(keyStart, "\r\n");
        if (keyEnd)
            wsKey.assign(keyStart, keyEnd - keyStart);
    }

    if (wsKey.empty()) {
        // 不是 WebSocket 请求，返回 400
        const char* badReq =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "WebSocket endpoint - use Upgrade: websocket";
        ::send(Sock, badReq, (int)strlen(badReq), 0);
        return false;
    }

    // 计算 Accept Key: BASE64(SHA1(key + GUID))
    std::string magicKey = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string hash = AuthSecurity::Sha1Raw(magicKey);
    std::string acceptKey = Base64Encode(hash);

    // 发送 101 Switching Protocols
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n";
    response << "Upgrade: websocket\r\n";
    response << "Connection: Upgrade\r\n";
    response << "Sec-WebSocket-Accept: " << acceptKey << "\r\n";
    response << "\r\n";

    std::string respStr = response.str();
    int sent = ::send(Sock, respStr.c_str(), (int)respStr.size(), 0);
    return sent == (int)respStr.size();
}

// ============================================================================
// WebSocket 连接处理 (独立线程)
// ============================================================================

static void WS_HandleConnection(SOCKET_TYPE Sock, FWebServer* pServer)
{
    printf("[WebServer] WebSocket client connected\n");

    // WebSocket 消息循环
    while (pServer->IsRunning()) {
        std::string payload;
        if (!WS_RecvFrame(Sock, payload, 30000)) {
            break;  // 超时或关闭
        }

        if (payload.empty()) continue;

        // 交给 WebServer 处理
        auto sendFunc = [Sock](const std::string& msg) -> bool {
            return WS_SendText(Sock, msg);
        };

        pServer->HandleWSMessage(payload, sendFunc);
    }

    closesocket(Sock);
    printf("[WebServer] WebSocket client disconnected\n");
}

// ============================================================================
// WebSocket 监听线程 (raw socket, 独立端口)
// ============================================================================

static void WS_ListenThreadFunc(FWebServer* pServer, uint16_t Port, const std::string& ListenIP)
{
    SOCKET_TYPE listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCK) {
        fprintf(stderr, "[WebServer] WS: Failed to create listen socket\n");
        return;
    }

    int reuse = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(Port);
    if (ListenIP == "0.0.0.0")
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, ListenIP.c_str(), &addr.sin_addr);

    if (::bind(listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCK_ERR) {
        fprintf(stderr, "[WebServer] WS: Failed to bind port %u\n", Port);
        closesocket(listenSock);
        return;
    }

    if (::listen(listenSock, SOMAXCONN) == SOCK_ERR) {
        fprintf(stderr, "[WebServer] WS: Failed to listen\n");
        closesocket(listenSock);
        return;
    }

    // 设置非阻塞
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(listenSock, FIONBIO, &nb);
#else
    int flags = fcntl(listenSock, F_GETFL, 0);
    fcntl(listenSock, F_SETFL, flags | O_NONBLOCK);
#endif

    printf("[WebServer] WebSocket listening on %s:%u\n", ListenIP.c_str(), Port);
    pServer->SetWsListening(true);

    while (pServer->IsRunning()) {
        struct sockaddr_in clientAddr = {};
        socklen_t clientLen = sizeof(clientAddr);
        SOCKET_TYPE clientSock = ::accept(listenSock, (struct sockaddr*)&clientAddr, &clientLen);

        if (clientSock == INVALID_SOCK) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK)
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            break;
        }

        // 重置为阻塞模式 (accept 继承监听 socket 的非阻塞属性)
#ifdef _WIN32
        u_long blockMode = 0;
        ioctlsocket(clientSock, FIONBIO, &blockMode);
#endif

        // WebSocket 握手
        if (!WS_PerformHandshake(clientSock, 5000)) {
            closesocket(clientSock);
            continue;
        }

        // 启动连接处理线程
        std::thread(WS_HandleConnection, clientSock, pServer).detach();
    }

    closesocket(listenSock);
    pServer->SetWsListening(false);
}

// ============================================================================
// FWebServer 构造与析构
// ============================================================================

FWebServer::FWebServer()
{
    m_RuntimeSettings.RegisterDefaultDefinitions();
    m_RuntimeSettings.RegisterObserver(this);
}

FWebServer::~FWebServer()
{
    Stop();
}

// ============================================================================
// 路由注册 (仅 HTTP 部分)
// ============================================================================

void FWebServer::SetupRoutes()
{
    if (!m_Server) return;

    // ---- 0. 静态文件服务 ----
    // 改用 cpp-httplib 内建挂载能力，避免自定义 GET fallback 在当前环境下持续返回 404。
    if (!m_Config.WwwDir.empty()) {
        if (!m_Server->set_mount_point("/", m_Config.WwwDir)) {
            fprintf(stderr, "[WebServer] Failed to mount static web root: %s\n",
                    m_Config.WwwDir.c_str());
        } else {
            printf("[WebServer] Static web root mounted: / -> %s\n",
                   m_Config.WwwDir.c_str());
        }
    }

    // ---- 1. 默认页面: 首页跳转到记账页面 ----
    m_Server->Get("/", [](const httplib::Request&, httplib::Response& Res) {
        Res.set_redirect("/ledger.html", 302);
    });

    // ---- 1. 认证 API: GET /api/login/precheck ----
    // 账号输入框失焦预检：检测账号是否存在以及是否为空密码。
    m_Server->Get("/api/login/precheck", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleLoginPrecheck(Req, Res);
    });

    // ---- 1. 认证 API: GET /api/login/challenge ----
    // 返回一次性 challenge 用于 challenge-response 登录
    m_Server->Get("/api/login/challenge", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleLoginChallenge(Req, Res);
    });

    // ---- 2. 认证 API: POST /api/login ----
    m_Server->Post("/api/login", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleLogin(Req, Res);
    });

    // ---- 2.5. POST 数据通道: POST /api/channel ----
    // 请求/响应协议与 WebSocket 完全一致: {"cmd":"...","token":"...","data":{...}}
    m_Server->Post("/api/channel", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleChannel(Req, Res);
    });

    // ---- 1.6. 外部数据通道: POST /data ----
    // 协议: {"cmd":"add","PID":"...","data":"语音识别文本"}，PID 也兼容小写 pid。
    m_Server->Post("/data", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleDataChannel(Req, Res);
    });

    // ---- 1.7. 通用上传入口：purpose=bill_import 时上传后直接触发账单导入 ----
    m_Server->Post("/api/upload", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleUpload(Req, Res);
    });

    // ---- 2. 认证 API: POST /api/logout ----
    m_Server->Post("/api/logout", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleLogout(Req, Res);
    });

    // ---- 3. 认证 API: GET /api/session ----
    m_Server->Get("/api/session", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleSession(Req, Res);
    });

    // ---- 3.2. 登录用户可读取的内置赞赏码图片 ----
    m_Server->Get("/api/donation/image", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleDonationImage(Req, Res);
    });

    // ---- 3.5. 邀请注册公开 API ----
    m_Server->Get("/api/register_invite", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleRegisterInvite(Req, Res);
    });
    m_Server->Post("/api/register_invite/join", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleRegisterInviteJoin(Req, Res);
    });
    m_Server->Post("/api/register", [this](const httplib::Request& Req, httplib::Response& Res) {
        HandleRegister(Req, Res);
    });

    // ---- 4. WebSocket 可选启用时由独立 raw socket 线程处理 (见 Start()) ----

    // ---- 5. 未知 API fallback：放在 API 路由之后，避免返回静态文件 ----
    m_Server->Get(R"(/api/(.*))", [](const httplib::Request&, httplib::Response& Res) {
        Res.status = 404;
        Res.set_content("{\"error\":\"not_found\",\"message\":\"Unknown API endpoint\"}",
                        "application/json; charset=utf-8");
    });

    // ---- 7. 未来预留: 客户端管理 REST API ----
    // m_Server->Post("/api/client/register", ...);
    // m_Server->Get("/api/clients", ...);
}

// ============================================================================
// 启动 / 停止
// HTTP 默认提供 POST /api/channel 数据通道；WebSocket 可选启用并使用独立端口
// ============================================================================

bool FWebServer::Start(const FWebServerConfig& Config)
{
    if (m_bRunning.load())
        return false;

    m_Config = Config;

    if (m_Config.WsPort == 0)
        m_Config.WsPort = m_Config.HttpPort + 1;
    if (m_Config.LedgerDbPath.empty())
        m_Config.LedgerDbPath = "./ledger.db";
    if (m_Config.WwwDir.empty())
        m_Config.WwwDir = "./www";

    bool bLedgerDbReady = false;

    // ---- 初始化账本数据库 ----
    {
        fs::path dbPath = fs::path(m_Config.LedgerDbPath);

        if (!m_LedgerManager.Initialize(dbPath.string())) {
            fprintf(stderr, "[WebServer] WARNING: Ledger database init failed, "
                    "ledger features disabled\n");
        } else {
            bLedgerDbReady = true;

            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            int cleanedInvites = m_LedgerManager.CleanupExpiredInviteRegistrations(now);
            if (cleanedInvites > 0) {
                printf("[WebServer] Cleaned %d expired invite registration(s)\n", cleanedInvites);
            }
            int cleanedPendingActions = m_LedgerManager.CleanupExpiredPendingActions(now);
            if (cleanedPendingActions > 0) {
                printf("[WebServer] Marked %d expired pending action(s)\n", cleanedPendingActions);
            }

            // 从数据库加载用户与运行时参数；RuntimeSettingManager 是后续热更新的唯一快照源。
            LoadUsersFromDatabase();
            ReloadRuntimeConfigFromDatabase();
        }
    }

    printf("[WebServer] Auth: max sessions per user = %d (%s)\n",
           m_Config.MaxSessionsPerUser,
           m_Config.MaxSessionsPerUser <= 0 ? "unlimited" : "limited");

    if (m_bAuthEnabled) {
        LoadSessionsFromDatabase();
    }

    if (!m_Config.bVoiceLedgerEnabled) {
        printf("[WebServer] VoiceLedger: disabled in system settings\n");
    } else {
        printf("[WebServer] VoiceLedger: DeepSeek endpoint=%s, model=%s, curl=%s, timeout=%d\n",
               m_Config.VoiceLedgerEndpoint.c_str(),
               m_Config.VoiceLedgerApiModel.c_str(),
               m_Config.VoiceLedgerCurlPath.c_str(),
               m_Config.VoiceLedgerTimeoutSec);
    }

    // ---- 初始化语音记账管理器 ----
    {
        FVoiceLedgerDeepSeekConfig voiceConfig = BuildVoiceLedgerConfigFromRuntimeSettings(m_RuntimeSettings);
        if (!m_VoiceLedgerManager.Initialize(voiceConfig, &m_LedgerManager)) {
            fprintf(stderr, "[WebServer] WARNING: VoiceLedger init failed. "
                    "Voice ledger API will return 'unavailable'.\n");
        } else {
            m_VoiceLedgerManager.UpdateConfig(voiceConfig, m_Config.bVoiceLedgerEnabled);
        }
    }

    // ---- 初始化账单导入管理器 ----
    if (!m_BillImportManager.Initialize(m_LedgerManager, m_VoiceLedgerManager)) {
        fprintf(stderr, "[WebServer] WARNING: BillImportManager worker init failed\n");
    }

    // 创建 httplib::Server (仅 HTTP)
    try {
        m_Server = std::make_unique<httplib::Server>();
    } catch (const std::exception& e) {
        fprintf(stderr, "[WebServer] Failed to create HTTP server: %s\n", e.what());
        return false;
    }

    SetupRoutes();

    m_Server->set_error_handler([](const httplib::Request&, httplib::Response& Res) {
        if (!Res.body.empty()) {
            return;
        }
        Res.set_content("Not Found", "text/plain");
    });

    m_bStopRequested.store(false);
    m_bRunning.store(true);

    // ---- 启动 HTTP 线程 (cpp-httplib) ----
    m_HttpThread = std::thread([this]() {
        printf("[WebServer] HTTP starting on %s:%u\n",
               m_Config.ListenIP.c_str(), m_Config.HttpPort);

        if (!m_Server->listen(m_Config.ListenIP.c_str(), m_Config.HttpPort)) {
            fprintf(stderr, "[WebServer] HTTP failed to listen on %s:%u\n",
                    m_Config.ListenIP.c_str(), m_Config.HttpPort);
            m_bRunning.store(false);
            return;
        }
        printf("[WebServer] HTTP stopped.\n");
    });

    // 等待 HTTP 启动
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (!m_Server->is_running()) {
        m_HttpThread.join();
        m_bRunning.store(false);
        return false;
    }

    // ---- 可选启动 WebSocket 线程 (raw socket, 独立端口) ----
    if (m_Config.bEnableWebSocket) {
        m_WsThread = std::thread(WS_ListenThreadFunc, this, m_Config.WsPort, m_Config.ListenIP);

        // 等待 WS 监听线程启动 (最多等 1 秒)
        for (int i = 0; i < 20 && !m_bWsListening.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!m_bWsListening.load()) {
            fprintf(stderr, "[WebServer] WARNING: WebSocket listener failed to start on port %u. "
                    "Check if port is available.\n", m_Config.WsPort);
        }
    } else {
        m_bWsListening.store(false);
        printf("[WebServer] WebSocket disabled. Use POST /api/channel as data channel.\n");
    }

    printf("[WebServer] Started: HTTP=%u, POST=/api/channel, WWW=%s, WS=%s",
           m_Config.HttpPort,
           m_Config.WwwDir.c_str(),
           m_Config.bEnableWebSocket ? (m_bWsListening.load() ? "listening" : "FAILED") : "disabled");
    if (m_Config.bEnableWebSocket) {
        printf(" (port %u)", m_Config.WsPort);
    }
    printf("\n");

    return true;
}

void FWebServer::Stop()
{
    if (!m_bRunning.load())
        return;

    printf("[WebServer] Stopping...\n");
    m_bStopRequested.store(true);
    m_bRunning.store(false);
    m_bWsListening.store(false);

    // 先停止语音记账工作线程（可能正在跑 LLM 推理）
    m_VoiceLedgerManager.Shutdown();

    // 停止账单导入工作线程
    m_BillImportManager.Shutdown();

    if (m_Server) {
        m_Server->stop();
    }

    if (m_HttpThread.joinable()) {
        m_HttpThread.join();
    }
    if (m_WsThread.joinable()) {
        m_WsThread.join();
    }

    printf("[WebServer] Stopped.\n");
}

// ============================================================================
// HTTP POST 数据通道: POST /api/channel
// 请求/响应协议与 WebSocket 完全一致
// ============================================================================

void FWebServer::HandleChannel(const httplib::Request& Req, httplib::Response& Res)
{
    std::string responseJson;
    bool bSent = false;

    auto sendFunc = [&responseJson, &bSent](const std::string& msg) -> bool {
        if (!bSent) {
            responseJson = msg;
            bSent = true;
        }
        return true;
    };

    try {
        HandleWSMessage(Req.body, sendFunc);
    } catch (const std::exception& e) {
        responseJson = BuildWSError("", std::string("Internal channel error: ") + e.what());
        bSent = true;
    } catch (...) {
        responseJson = BuildWSError("", "Internal channel error");
        bSent = true;
    }

    if (!bSent) {
        responseJson = BuildWSError("", "No response generated");
    }

    Res.set_content(responseJson, "application/json");
}

int FWebServer::ResolveDefaultLedgerIdForUser(const std::string& Username)
{
    int currentLedgerId = 0;
    if (m_LedgerManager.GetCurrentLedgerId(Username, currentLedgerId)
        && currentLedgerId > 0) {
        return currentLedgerId;
    }
    return 0;
}

bool FWebServer::GetCurrentRequestUsername(std::string& OutUsername) const
{
    OutUsername = s_RequestAuthContext.Username;
    return s_RequestAuthContext.bAuthenticated && !OutUsername.empty();
}

bool FWebServer::CurrentRequestHasPermission(const std::string& PermissionName,
                                             std::string* OutUsername) const
{
    if (!s_RequestAuthContext.bAuthenticated || s_RequestAuthContext.Username.empty()) {
        return false;
    }

    const std::string target = "\"" + PermissionName + "\"";
    const bool hasPermission = s_RequestAuthContext.Permissions.find(target) != std::string::npos;
    if (hasPermission && OutUsername) {
        *OutUsername = s_RequestAuthContext.Username;
    }
    return hasPermission;
}

bool FWebServer::EnsurePermissionForRequest(const std::string& PermissionName,
                                            const std::string& Cmd,
                                            WS_SendFunc Send,
                                            std::string* OutUsername) const
{
    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty() && m_bAuthEnabled) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return false;
    }
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return false;
    }

    if (!CurrentRequestHasPermission(PermissionName, OutUsername)) {
        Send(BuildWSError(Cmd, "Current user does not have required permission: " + PermissionName));
        return false;
    }
    return true;
}

bool FWebServer::EnsureAdminForRequest(const std::string& Cmd,
                                       WS_SendFunc Send,
                                       std::string* OutUsername) const
{
    return EnsurePermissionForRequest("admin", Cmd, Send, OutUsername);
}

bool FWebServer::EnsureUserForRequest(const std::string& Cmd,
                                      WS_SendFunc Send,
                                      std::string* OutUsername) const
{
    return EnsurePermissionForRequest("user", Cmd, Send, OutUsername);
}

bool FWebServer::EnsureGroupMemberForRequest(int GroupId, const std::string& Cmd,
                                             WS_SendFunc Send,
                                             std::string* OutUsername)
{
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return false;
    }

    if (!m_LedgerManager.IsGroupMember(GroupId, username)) {
        Send(BuildWSError(Cmd, "Current user is not a member of groupId"));
        return false;
    }

    if (OutUsername) *OutUsername = username;
    return true;
}

void FWebServer::HandleDataChannel(const httplib::Request& Req, httplib::Response& Res)
{
    std::string responseBody;
    bool bSent = false;

    std::string cmd;
    JsonGetStringValueStrict(Req.body, "cmd", cmd);
    if (cmd.empty()) {
        Res.set_content(BuildWSError("", "Missing 'cmd' field"), "application/json");
        return;
    }

    std::string text;
    JsonGetStringValueStrict(Req.body, "data", text);
    if (text.empty()) {
        Res.set_content(BuildWSError(cmd, "Missing 'data' voice text field"), "application/json");
        return;
    }

    std::string pid;
    JsonGetStringValueStrict(Req.body, "PID", pid);
    if (pid.empty()) {
        JsonGetStringValueStrict(Req.body, "pid", pid);
    }
    if (cmd != "add") {
        Res.set_content(BuildWSError(cmd, "Unknown /data command: " + cmd), "application/json");
        return;
    }

    FLedgerPidRoute route;
    std::string routeError;
    if (pid.empty() || !m_LedgerManager.ResolveLedgerPidRoute(pid, route, routeError)) {
        Res.set_content("未设置好语音通道", "text/plain; charset=utf-8");
        return;
    }

    // PID 已携带稳定的账本与创建者绑定，外部写入绝不依赖用户当前账本。
    FScopedWorkLogUserContext logContext(route.CreatorUserId);

    try {
        int transactionId = 0;
        std::string error;
        if (!m_LedgerManager.CreateVoiceInputTransaction(route.LedgerId, text,
                                                         route.CreatorUsername,
                                                         transactionId, error)) {
            Res.set_content(BuildWSError(cmd, error.empty() ? "Failed to create voice transaction" : error),
                            "application/json");
            return;
        }

        responseBody = std::string("已记录,") + text;
        bSent = true;
    } catch (const std::exception& e) {
        responseBody = BuildWSError(cmd, std::string("Internal data channel error: ") + e.what());
        bSent = true;
    } catch (...) {
        responseBody = BuildWSError(cmd, "Internal data channel error");
        bSent = true;
    }

    if (!bSent) {
        responseBody = BuildWSError(cmd, "No response generated");
    }

    const bool bPlainTextResponse = bSent;
    Res.set_content(responseBody,
                    bPlainTextResponse ? "text/plain; charset=utf-8" : "application/json");
}

void FWebServer::HandleUpload(const httplib::Request& Req, httplib::Response& Res)
{
    auto jsonError = [&](int Status, const std::string& Message) {
        Res.status = Status;
        Res.set_content(std::string("{\"ok\":false,\"error\":\"") + EscapeJsonString(Message) + "\"}",
                        "application/json; charset=utf-8");
    };

    auto getUploadField = [&](const std::string& Name) -> std::string {
        if (Req.has_param(Name)) return Req.get_param_value(Name);
        if (Req.form.has_field(Name)) return Req.form.get_field(Name);
        if (Req.form.has_file(Name)) return Req.form.get_file(Name).content;
        return "";
    };

    std::string token;
    if (Req.has_header("Authorization")) {
        std::string authHeader = Req.get_header_value("Authorization");
        const char* prefix = "Bearer ";
        if (authHeader.size() >= strlen(prefix) && authHeader.compare(0, strlen(prefix), prefix) == 0) {
            token = authHeader.substr(strlen(prefix));
        }
    }
    if (token.empty()) token = getUploadField("token");
    if (!CheckAuth(Req, Res, token)) return;

    std::string username = "anonymous";
    if (m_bAuthEnabled) {
        std::lock_guard<std::mutex> lock(m_SessionMutex);
        auto it = m_Sessions.find(token);
        if (it == m_Sessions.end()) { jsonError(401, "Invalid or missing token"); return; }
        username = it->second.Username;
    }

    std::string purpose = getUploadField("purpose");
    if (purpose.empty()) { jsonError(400, "Missing upload purpose"); return; }
    if (!Req.form.has_file("file")) { jsonError(400, "Missing upload file"); return; }
    const auto upload = Req.form.get_file("file");
    const size_t kMaxUploadBytes = 10u * 1024u * 1024u;
    if (upload.content.empty()) { jsonError(400, "Uploaded file is empty"); return; }
    if (upload.content.size() > kMaxUploadBytes) { jsonError(413, "Uploaded file is too large"); return; }
    if (purpose != "bill_import") { jsonError(400, "Unsupported upload purpose"); return; }

    int ledgerId = 0;
    const std::string ledgerIdText = getUploadField("ledgerId");
    if (!ledgerIdText.empty()) {
        ledgerId = atoi(ledgerIdText.c_str());
    }
    if (ledgerId <= 0) {
        // 当前账本仅作为请求未显式指定作用域时的 UI 缺省值，授权仍由领域层校验。
        ledgerId = ResolveDefaultLedgerIdForUser(username);
    }
    if (ledgerId <= 0) {
        jsonError(400, "Missing ledgerId");
        return;
    }

    // 异步任务创建接口会再次校验账本所属家庭成员关系，HTTP 层不复制授权规则。
    int taskId = m_LedgerManager.CreateBillImportTask(ledgerId, username,
                                                       upload.filename, upload.content);
    if (taskId <= 0) {
        jsonError(500, "创建导入任务失败");
        return;
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"taskId\":" << taskId
        << ",\"status\":\"pending\""
        << ",\"message\":\"上传完成，等待导入处理\"}";
    Res.set_content(out.str(), "application/json; charset=utf-8");
}

// ============================================================================
// 通用数据通道消息分发 (公开接口，供 raw socket WS handler 与 POST channel 调用)
// ============================================================================

void FWebServer::HandleWSMessage(const std::string& MsgJson, WS_SendFunc Send)
{
    std::string cmd = JsonGetString(MsgJson, "cmd");
    if (cmd.empty()) {
        Send(BuildWSError("", "Missing 'cmd' field"));
        return;
    }

    // 提取 data JSON 对象。
    std::string dataStr;
    std::string search = "\"data\"";
    size_t pos = MsgJson.find(search);
    if (pos != std::string::npos) {
        pos = MsgJson.find(':', pos + search.size());
        if (pos != std::string::npos) {
            pos++;
            while (pos < MsgJson.size() && (MsgJson[pos] == ' ' || MsgJson[pos] == '\t'))
                pos++;
            if (pos < MsgJson.size() && MsgJson[pos] == '{') {
                int braceCount = 0;
                size_t start = pos;
                for (; pos < MsgJson.size(); ++pos) {
                    if (MsgJson[pos] == '{') braceCount++;
                    else if (MsgJson[pos] == '}') {
                        braceCount--;
                        if (braceCount == 0) {
                            dataStr = MsgJson.substr(start, pos - start + 1);
                            break;
                        }
                    }
                }
            }
        }
    }

    // ---- 认证检查 ----
    // 每个请求先重置本线程上下文，避免线程池复用工作线程时继承上一次请求身份。
    s_RequestAuthContext = FAuthRequestContext{};
    if (m_bAuthEnabled) {
        const std::string token = JsonGetString(MsgJson, "token");
        std::string username;
        {
            std::lock_guard<std::mutex> lock(m_SessionMutex);
            const auto it = m_Sessions.find(token);
            if (it != m_Sessions.end()) {
                username = it->second.Username;
            }
        }
        if (username.empty()) {
            Send(BuildWSError(cmd, "Authentication required: invalid or expired token"));
            return;
        }

        // 权限在请求入口形成快照，后续处理器只读取当前线程上下文，不再访问共享请求身份。
        s_RequestAuthContext.Token = token;
        s_RequestAuthContext.Username = username;
        s_RequestAuthContext.Permissions = GetUserPermissions(username);
        s_RequestAuthContext.bAuthenticated = true;
    }

    if (cmd == "ledger.user.list") {
        HandleLedgerUserList(dataStr, cmd, Send);
    } else if (cmd == "ledger.user.create") {
        HandleLedgerUserCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.user.update_pwd") {
        HandleLedgerUserUpdatePwd(dataStr, cmd, Send);
    } else if (cmd == "ledger.user.set_active") {
        HandleLedgerUserSetActive(dataStr, cmd, Send);
    } else if (cmd == "ledger.pid.list") {
        HandleLedgerPidList(dataStr, cmd, Send);
    } else if (cmd == "ledger.pid.create") {
        HandleLedgerPidCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.pid.update_ledger") {
        HandleLedgerPidUpdateLedger(dataStr, cmd, Send);
    } else if (cmd == "ledger.pid.delete") {
        HandleLedgerPidDelete(dataStr, cmd, Send);
    } else if (cmd == "ledger.user.update_pid") {
        Send(BuildWSError(cmd, "PID_API_DEPRECATED: use ledger.pid.create with ledgerId"));
    } else if (cmd == "ledger.user.profile") {
        HandleLedgerUserProfile(dataStr, cmd, Send);
    } else if (cmd == "ledger.user.delete") {
        HandleLedgerUserDelete(dataStr, cmd, Send);
    } else if (cmd == "ledger.settings.list") {
        HandleLedgerSystemSettingsList(dataStr, cmd, Send);
    } else if (cmd == "ledger.settings.update") {
        HandleLedgerSystemSettingsUpdate(dataStr, cmd, Send);
    } else if (cmd == "ledger.settings.delete") {
        HandleLedgerSystemSettingsDelete(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.list") {
        HandleLedgerGroupList(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.create") {
        HandleLedgerGroupCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.delete") {
        HandleLedgerGroupDelete(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.set_current") {
        HandleLedgerGroupSetCurrent(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.members") {
        HandleLedgerGroupMembers(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.add_member") {
        HandleLedgerGroupAddMember(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.invite_member") {
        HandleLedgerGroupInviteMember(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.invites.incoming") {
        HandleLedgerGroupIncomingInvites(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.invites.sent") {
        HandleLedgerGroupSentInvites(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.invite.accept") {
        HandleLedgerGroupAcceptInvite(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.invite.reject") {
        HandleLedgerGroupRejectInvite(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.invite.cancel") {
        HandleLedgerGroupCancelInvite(dataStr, cmd, Send);
    } else if (cmd == "ledger.group.remove_member") {
        HandleLedgerGroupRemoveMember(dataStr, cmd, Send);
    } else if (cmd == "ledger.category.list") {
        HandleLedgerCategoryList(dataStr, cmd, Send);
    } else if (cmd == "ledger.category.create") {
        HandleLedgerCategoryCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.category.update") {
        HandleLedgerCategoryUpdate(dataStr, cmd, Send);
    } else if (cmd == "ledger.category.delete") {
        HandleLedgerCategoryDelete(dataStr, cmd, Send);
    } else if (cmd == "ledger.transaction.create") {
        HandleLedgerTransactionCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.transaction.list") {
        HandleLedgerTransactionList(dataStr, cmd, Send);
    } else if (cmd == "ledger.transaction.update") {
        HandleLedgerTransactionUpdate(dataStr, cmd, Send);
    } else if (cmd == "ledger.transaction.delete") {
        HandleLedgerTransactionDelete(dataStr, cmd, Send);
    } else if (cmd == "ledger.transaction.stats") {
        HandleLedgerTransactionStats(dataStr, cmd, Send);
    } else if (cmd == "ledger.voice.create") {
        HandleLedgerVoiceCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.voice.test") {
        HandleLedgerVoiceTest(dataStr, cmd, Send);
    } else if (cmd == "ledger.voice.parse") {
        HandleLedgerVoiceParse(dataStr, cmd, Send);
    } else if (cmd == "ledger.invite.create") {
        HandleLedgerInviteCreate(dataStr, cmd, Send);
    } else if (cmd == "ledger.invite.current") {
        HandleLedgerInviteCurrent(dataStr, cmd, Send);
    } else if (cmd == "ledger.bill_import.status") {
        HandleLedgerBillImportStatus(dataStr, cmd, Send);
    }
    else {
        Send(BuildWSError(cmd, "Unknown command: " + cmd));
    }
}

// ============================================================================
// 认证：从 SQLite 数据库加载用户到内存
// ============================================================================

bool FWebServer::LoadUsersFromDatabase()
{
    m_AuthUsers.clear();

    std::string usersJson;
    if (!m_LedgerManager.GetAllUsers(usersJson)) {
        printf("[WebServer] Auth: failed to query users from database\n");
        return false;
    }

    if (usersJson == "[]") {
        printf("[WebServer] Auth: no users in database, auth disabled\n");
        m_bAuthEnabled = false;
        return false;
    }

    // 解析 JSON 数组并填充 m_AuthUsers
    size_t pos = 0;
    int loaded = 0;

    while (true) {
        pos = usersJson.find('{', pos);
        if (pos == std::string::npos) break;

        int depth = 0;
        size_t start = pos;
        for (; pos < usersJson.size(); ++pos) {
            if (usersJson[pos] == '{') depth++;
            else if (usersJson[pos] == '}') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (pos >= usersJson.size()) break;

        std::string obj = usersJson.substr(start, pos - start + 1);
        pos++;

        std::string username = JsonGetString(obj, "username");
        std::string permissionsRaw = JsonGetArrayRaw(obj, "permissions");
        std::string permissions = permissionsRaw.empty() ? "[\"user\"]" : ("[" + permissionsRaw + "]");
        if (permissions.find("\"user\"") == std::string::npos) {
            if (permissions == "[]") {
                permissions = "[\"user\"]";
            } else {
                permissions.insert(permissions.size() - 1, ",\"user\"");
            }
        }

        if (username.empty() || JsonGetInt(obj, "isActive", 0) == 0) continue;

        FAuthUser user;
        user.Username = username;
        user.PasswordHash = "";
        user.Permissions = permissions;
        m_AuthUsers[username] = user;
        loaded++;
    }

    if (loaded > 0) {
        m_bAuthEnabled = true;
        printf("[WebServer] Auth: loaded %d user(s) from database\n", loaded);
        return true;
    }

    m_bAuthEnabled = false;
    return false;
}

std::string FWebServer::GetObserverName() const
{
    return "WebServer";
}

bool FWebServer::CanHandleSetting(const std::string& Key) const
{
    return Key == "server.listenIp" ||
           Key == "server.httpPort" ||
           Key == "server.wwwDir" ||
           Key == "auth.maxSessionsPerUser" ||
           Key == "ledger.memberInviteExpiresSec" ||
           Key == "voiceLedger.enabled" ||
           Key == "voiceLedger.apiKey" ||
           Key == "voiceLedger.endpoint" ||
           Key == "voiceLedger.model" ||
           Key == "voiceLedger.curlPath" ||
           Key == "voiceLedger.timeoutSec" ||
           Key == "voiceLedger.temperature" ||
           Key == "voiceLedger.maxTokens";
}

FRuntimeSettingApplyResult FWebServer::OnRuntimeSettingChanged(
    const FRuntimeSettingChangeEvent& Event,
    const RuntimeSettingManager& Manager)
{
    FRuntimeSettingApplyResult result;
    result.bOk = true;
    result.ApplyMode = Event.ApplyMode;
    result.EffectiveValue = Event.EffectiveValue;
    result.bRestartRequired = Event.bRestartRequired;

    if (Event.ApplyMode == ERuntimeSettingApplyMode::RestartRequired) {
        // 监听地址与端口属于 socket 生命周期边界，只更新快照，不伪装成运行中切换监听。
        result.bApplied = false;
        result.bRestartRequired = true;
        result.Message = "Saved. Restart YuanBook service to apply network binding changes.";
        return result;
    }

    ApplySystemSettingValue(Event.SettingKey, Event.EffectiveValue);

    if (Event.SettingKey == "auth.maxSessionsPerUser") {
        const int removed = PruneAllUsersOldSessions();
        std::ostringstream oss;
        oss << "Applied immediately. Pruned " << removed << " old session(s).";
        result.Message = oss.str();
        result.bApplied = true;
        return result;
    }

    if (Event.Category == "voiceLedger" || Event.SettingKey.find("voiceLedger.") == 0) {
        ApplyVoiceLedgerRuntimeConfig(Manager);
        result.Message = "Applied immediately. Voice ledger runtime config reloaded.";
        result.bApplied = true;
        return result;
    }

    if (Event.SettingKey == "server.wwwDir") {
        result.Message = "Saved. Static web root will be used by subsequent static file handling where supported.";
        result.bApplied = true;
        return result;
    }

    result.Message = "Applied immediately.";
    result.bApplied = true;
    return result;
}

FRuntimeSettingApplyResult FWebServer::NotifySystemSettingChanged(const FSystemSettingChangedEvent& Event,
                                                                  bool bDeleted)
{
    return bDeleted ? m_RuntimeSettings.ApplyDeleteEvent(Event)
                    : m_RuntimeSettings.ApplyUpsertEvent(Event);
}

void FWebServer::ApplySystemSettingValue(const std::string& SettingKey,
                                         const std::string& SettingValue)
{
    if (SettingKey == "server.listenIp") {
        if (!SettingValue.empty()) {
            m_Config.ListenIP = SettingValue;
        } else {
            m_Config.ListenIP = "0.0.0.0";
        }
        return;
    }

    if (SettingKey == "server.httpPort") {
        try {
            int port = std::stoi(SettingValue);
            if (port > 0 && port <= 65535) {
                m_Config.HttpPort = static_cast<uint16_t>(port);
            } else {
                m_Config.HttpPort = 5080;
            }
        } catch (...) {
            m_Config.HttpPort = 5080;
        }
        return;
    }

    if (SettingKey == "server.wwwDir") {
        if (!SettingValue.empty()) {
            m_Config.WwwDir = SystemUtils::ResolveRuntimePath(SettingValue, m_Config.WwwDir);
        } else {
            m_Config.WwwDir = SystemUtils::ResolveRuntimePath("./www", m_Config.WwwDir);
        }
        return;
    }

    if (SettingKey == "auth.maxSessionsPerUser") {
        try {
            m_Config.MaxSessionsPerUser = std::stoi(SettingValue);
        } catch (...) {
            m_Config.MaxSessionsPerUser = 5;
        }
        return;
    }

    if (SettingKey == "voiceLedger.enabled") {
        m_Config.bVoiceLedgerEnabled = JsonLite::GetBoolOrDefault("{\"v\":" + SettingValue + "}", "v", m_Config.bVoiceLedgerEnabled);
        return;
    }

    if (SettingKey == "voiceLedger.apiKey") {
        m_Config.VoiceLedgerApiKey = SettingValue;
        return;
    }

    if (SettingKey == "voiceLedger.endpoint") {
        if (!SettingValue.empty()) {
            m_Config.VoiceLedgerEndpoint = SettingValue;
        }
        return;
    }

    if (SettingKey == "voiceLedger.model") {
        if (!SettingValue.empty()) {
            m_Config.VoiceLedgerApiModel = SettingValue;
        }
        return;
    }

    if (SettingKey == "voiceLedger.curlPath") {
        if (!SettingValue.empty()) {
            m_Config.VoiceLedgerCurlPath = SettingValue;
        }
        return;
    }

    if (SettingKey == "voiceLedger.timeoutSec") {
        try {
            m_Config.VoiceLedgerTimeoutSec = std::stoi(SettingValue);
        } catch (...) {
            m_Config.VoiceLedgerTimeoutSec = 60;
        }
        return;
    }

    if (SettingKey == "voiceLedger.temperature") {
        try {
            m_Config.VoiceLedgerTemperature = std::stod(SettingValue);
        } catch (...) {
            m_Config.VoiceLedgerTemperature = 0.1;
        }
        return;
    }

    if (SettingKey == "voiceLedger.maxTokens") {
        try {
            m_Config.VoiceLedgerMaxTokens = std::stoi(SettingValue);
        } catch (...) {
            m_Config.VoiceLedgerMaxTokens = 512;
        }
        return;
    }
}

void FWebServer::ReloadRuntimeConfigFromDatabase()
{
    m_RuntimeSettings.LoadFromLedger(m_LedgerManager);

    ApplySystemSettingValue("server.listenIp", m_RuntimeSettings.GetString("server.listenIp", m_Config.ListenIP));
    if (!m_Config.bHttpPortFromCommandLine) {
        ApplySystemSettingValue("server.httpPort", m_RuntimeSettings.GetString("server.httpPort", std::to_string(m_Config.HttpPort)));
    }
    ApplySystemSettingValue("server.wwwDir", m_RuntimeSettings.GetString("server.wwwDir", m_Config.WwwDir));
    ApplySystemSettingValue("auth.maxSessionsPerUser", m_RuntimeSettings.GetString("auth.maxSessionsPerUser", std::to_string(m_Config.MaxSessionsPerUser)));
    ApplySystemSettingValue("voiceLedger.enabled", m_RuntimeSettings.GetString("voiceLedger.enabled", m_Config.bVoiceLedgerEnabled ? "true" : "false"));
    ApplySystemSettingValue("voiceLedger.apiKey", m_RuntimeSettings.GetString("voiceLedger.apiKey", m_Config.VoiceLedgerApiKey));
    ApplySystemSettingValue("voiceLedger.endpoint", m_RuntimeSettings.GetString("voiceLedger.endpoint", m_Config.VoiceLedgerEndpoint));
    ApplySystemSettingValue("voiceLedger.model", m_RuntimeSettings.GetString("voiceLedger.model", m_Config.VoiceLedgerApiModel));
    ApplySystemSettingValue("voiceLedger.curlPath", m_RuntimeSettings.GetString("voiceLedger.curlPath", m_Config.VoiceLedgerCurlPath));
    ApplySystemSettingValue("voiceLedger.timeoutSec", m_RuntimeSettings.GetString("voiceLedger.timeoutSec", std::to_string(m_Config.VoiceLedgerTimeoutSec)));
    ApplySystemSettingValue("voiceLedger.temperature", m_RuntimeSettings.GetString("voiceLedger.temperature", std::to_string(m_Config.VoiceLedgerTemperature)));
    ApplySystemSettingValue("voiceLedger.maxTokens", m_RuntimeSettings.GetString("voiceLedger.maxTokens", std::to_string(m_Config.VoiceLedgerMaxTokens)));
}

FVoiceLedgerDeepSeekConfig FWebServer::BuildVoiceLedgerConfigFromRuntimeSettings(const RuntimeSettingManager& Manager) const
{
    FVoiceLedgerDeepSeekConfig config;
    config.Endpoint = Manager.GetString("voiceLedger.endpoint", m_Config.VoiceLedgerEndpoint);
    config.Model = Manager.GetString("voiceLedger.model", m_Config.VoiceLedgerApiModel);
    config.ApiKey = Manager.GetString("voiceLedger.apiKey", m_Config.VoiceLedgerApiKey);
    config.CurlPath = Manager.GetString("voiceLedger.curlPath", m_Config.VoiceLedgerCurlPath);
    config.TimeoutSec = Manager.GetInt("voiceLedger.timeoutSec", m_Config.VoiceLedgerTimeoutSec);
    config.Temperature = Manager.GetDouble("voiceLedger.temperature", m_Config.VoiceLedgerTemperature);
    config.MaxTokens = Manager.GetInt("voiceLedger.maxTokens", m_Config.VoiceLedgerMaxTokens);
    return config;
}

void FWebServer::ApplyVoiceLedgerRuntimeConfig(const RuntimeSettingManager& Manager)
{
    const FVoiceLedgerDeepSeekConfig voiceConfig = BuildVoiceLedgerConfigFromRuntimeSettings(Manager);
    const bool bEnabled = Manager.GetBool("voiceLedger.enabled", m_Config.bVoiceLedgerEnabled);
    m_Config.bVoiceLedgerEnabled = bEnabled;
    m_Config.VoiceLedgerApiKey = voiceConfig.ApiKey;
    m_Config.VoiceLedgerEndpoint = voiceConfig.Endpoint;
    m_Config.VoiceLedgerApiModel = voiceConfig.Model;
    m_Config.VoiceLedgerCurlPath = voiceConfig.CurlPath;
    m_Config.VoiceLedgerTimeoutSec = voiceConfig.TimeoutSec;
    m_Config.VoiceLedgerTemperature = voiceConfig.Temperature;
    m_Config.VoiceLedgerMaxTokens = voiceConfig.MaxTokens;
    m_VoiceLedgerManager.UpdateConfig(voiceConfig, bEnabled);
}

int FWebServer::PruneAllUsersOldSessions()
{
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    int removed = 0;
    for (const auto& userPair : m_AuthUsers) {
        removed += PruneOldSessionsForUserLocked(userPair.first);
    }
    return removed;
}

void FWebServer::HandleLedgerSystemSettingsList(const std::string& DataStr,
                                                const std::string& Cmd, WS_SendFunc Send)
{
    (void)DataStr;
    if (!EnsureAdminForRequest(Cmd, Send)) {
        return;
    }

    std::string outJson;
    if (m_LedgerManager.GetSystemSettings(outJson)) {
        // 管理后台属于前端可见面，服务端强制过滤历史库中遗留的 WS 参数，避免旧数据透出。
        Send(BuildWSResponse(Cmd, std::string("{\"settings\":") + outJson + "}"));
    } else {
        Send(BuildWSError(Cmd, "Failed to query system settings"));
    }
}

void FWebServer::HandleLedgerSystemSettingsUpdate(const std::string& DataStr,
                                                  const std::string& Cmd, WS_SendFunc Send)
{
    std::string updatedBy;
    if (!EnsureAdminForRequest(Cmd, Send, &updatedBy)) {
        return;
    }

    FSystemSettingItem setting;
    setting.SettingKey = JsonGetString(DataStr, "key");
    if (setting.SettingKey.empty()) {
        setting.SettingKey = JsonGetString(DataStr, "settingKey");
    }
    if (setting.SettingKey.empty()) {
        Send(BuildWSError(Cmd, "Missing settingKey"));
        return;
    }
    if (setting.SettingKey == "server.wsPort" || setting.SettingKey == "server.enableWebSocket") {
        Send(BuildWSError(Cmd, "Unsupported system setting"));
        return;
    }

    std::string value;
    if (!JsonGetStringValueStrict(DataStr, "value", value)) {
        Send(BuildWSError(Cmd, "Missing string field: value"));
        return;
    }
    setting.SettingValue = value;

    std::string validateMessage;
    if (!m_RuntimeSettings.ValidateRawValue(setting.SettingKey, setting.SettingValue, validateMessage)) {
        Send(BuildWSError(Cmd, validateMessage.empty() ? "Invalid system setting value" : validateMessage));
        return;
    }

    if (!m_RuntimeSettings.BuildSettingItemWithDefinition(setting)) {
        Send(BuildWSError(Cmd, "Unknown system setting"));
        return;
    }

    FSystemSettingChangedEvent event;
    if (!m_LedgerManager.UpsertSystemSetting(setting, updatedBy, &event)) {
        Send(BuildWSError(Cmd, "Failed to update system setting"));
        return;
    }

    FRuntimeSettingApplyResult applyResult = NotifySystemSettingChanged(event, false);

    std::ostringstream data;
    data << "{\"ok\":" << (applyResult.bOk ? "true" : "false")
         << ",\"settingKey\":\"" << EscapeJsonString(event.SettingKey) << "\""
         << ",\"updatedBy\":\"" << EscapeJsonString(event.UpdatedBy) << "\""
         << ",\"updatedAtUnix\":" << event.UpdatedAtUnix;
    AppendRuntimeSettingApplyResultJson(data, applyResult);
    data << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

void FWebServer::HandleLedgerSystemSettingsDelete(const std::string& DataStr,
                                                  const std::string& Cmd, WS_SendFunc Send)
{
    std::string updatedBy;
    if (!EnsureAdminForRequest(Cmd, Send, &updatedBy)) {
        return;
    }

    std::string settingKey = JsonGetString(DataStr, "key");
    if (settingKey.empty()) {
        settingKey = JsonGetString(DataStr, "settingKey");
    }
    if (settingKey.empty()) {
        Send(BuildWSError(Cmd, "Missing settingKey"));
        return;
    }
    if (settingKey == "server.wsPort" || settingKey == "server.enableWebSocket") {
        Send(BuildWSError(Cmd, "Unsupported system setting"));
        return;
    }

    std::string oldValue;
    if (!m_LedgerManager.DeleteSystemSetting(settingKey, oldValue)) {
        Send(BuildWSError(Cmd, "Failed to delete system setting"));
        return;
    }

    FSystemSettingChangedEvent event;
    event.SettingKey = settingKey;
    event.OldValue = oldValue;
    event.NewValue.clear();
    event.UpdatedBy = updatedBy;
    event.UpdatedAtUnix = static_cast<int64_t>(std::time(nullptr));
    FRuntimeSettingApplyResult applyResult = NotifySystemSettingChanged(event, true);

    std::ostringstream data;
    data << "{\"ok\":" << (applyResult.bOk ? "true" : "false")
         << ",\"settingKey\":\"" << EscapeJsonString(event.SettingKey) << "\""
         << ",\"updatedBy\":\"" << EscapeJsonString(event.UpdatedBy) << "\""
         << ",\"updatedAtUnix\":" << event.UpdatedAtUnix;
    AppendRuntimeSettingApplyResultJson(data, applyResult);
    data << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.user.list
// ============================================================================

void FWebServer::HandleLedgerUserList(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send)
{
    if (!EnsureAdminForRequest(Cmd, Send)) {
        return;
    }

    std::string outJson;
    if (m_LedgerManager.GetAllUsers(outJson)) {
        std::ostringstream data;
        data << "{\"users\":" << outJson << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, "Failed to query users"));
    }
}

// ============================================================================
// 记账 API: ledger.user.create
// ============================================================================

void FWebServer::HandleLedgerUserCreate(const std::string& DataStr,
                                         const std::string& Cmd, WS_SendFunc Send)
{
    std::string username = JsonGetString(DataStr, "username");
    std::string password = JsonGetString(DataStr, "password");
    std::string permissions;

    // 解析 permissions 数组
    std::string permsRaw = JsonGetArrayRaw(DataStr, "permissions");
    permissions = permsRaw.empty() ? "[\"user\"]" : ("[" + permsRaw + "]");
    if (permissions.find("\"user\"") == std::string::npos) {
        if (permissions == "[]") {
            permissions = "[\"user\"]";
        } else {
            permissions.insert(permissions.size() - 1, ",\"user\"");
        }
    }

    if (!EnsureAdminForRequest(Cmd, Send)) {
        return;
    }

    if (username.empty()) {
        Send(BuildWSError(Cmd, "Missing username"));
        return;
    }

    std::string passwordHash = AuthSecurity::HashPasswordForStorage(password);
    if (m_LedgerManager.CreateUser(username, passwordHash, permissions)) {
        // 同步到内存
        {
            FAuthUser u;
            u.Username = username;
            u.PasswordHash = passwordHash;
            u.Permissions = m_LedgerManager.IsUserAdmin(username) ? permissions : permissions;
            m_AuthUsers[username] = u;
        }
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, "Failed to create user. Username may already exist."));
    }
}

// ============================================================================
// 记账 API: ledger.user.update_pwd
// ============================================================================

void FWebServer::HandleLedgerUserUpdatePwd(const std::string& DataStr,
                                            const std::string& Cmd, WS_SendFunc Send)
{
    std::string actorUsername;
    if (!EnsureAdminForRequest(Cmd, Send, &actorUsername)) {
        return;
    }

    const std::string username = JsonGetString(DataStr, "username");
    const std::string password = JsonGetString(DataStr, "password");
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Missing username"));
        return;
    }

    const std::string passwordHash = AuthSecurity::HashPasswordForStorage(password);
    LedgerManager::FUserAccountMutationResult result;
    if (!m_LedgerManager.AdminUpdateUserPassword(actorUsername, username, passwordHash, result)) {
        Send(BuildWSError(Cmd, result.ErrorCode + ": " + result.Message));
        return;
    }

    auto it = m_AuthUsers.find(username);
    if (it != m_AuthUsers.end()) {
        it->second.PasswordHash = passwordHash;
    }
    RemoveUserSessionsFromMemory(username);

    std::ostringstream data;
    data << "{\"ok\":true,\"deletedSessionCount\":" << result.DeletedSessionCount
         << ",\"passwordCleared\":" << (passwordHash.empty() ? "true" : "false") << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

void FWebServer::HandleLedgerUserSetActive(const std::string& DataStr,
                                            const std::string& Cmd, WS_SendFunc Send)
{
    std::string actorUsername;
    if (!EnsureAdminForRequest(Cmd, Send, &actorUsername)) {
        return;
    }

    const std::string username = JsonGetString(DataStr, "username");
    const bool bActive = JsonGetInt(DataStr, "active", 0) != 0;
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Missing username"));
        return;
    }

    LedgerManager::FUserAccountMutationResult result;
    if (!m_LedgerManager.SetUserActiveState(actorUsername, username, bActive, result)) {
        Send(BuildWSError(Cmd, result.ErrorCode + ": " + result.Message));
        return;
    }

    if (bActive) {
        LoadUsersFromDatabase();
    } else {
        m_AuthUsers.erase(username);
        RemoveUserSessionsFromMemory(username);
    }

    std::ostringstream data;
    data << "{\"ok\":true,\"isActive\":" << (bActive ? "true" : "false")
         << ",\"deletedSessionCount\":" << result.DeletedSessionCount << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

void FWebServer::HandleLedgerPidList(const std::string& DataStr,
                                     const std::string& Cmd, WS_SendFunc Send)
{
    (void)DataStr;
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) return;
    std::string bindingsJson;
    if (!m_LedgerManager.GetLedgerPidBindings(username, bindingsJson)) {
        Send(BuildWSError(Cmd, "Failed to query PID bindings"));
        return;
    }
    Send(BuildWSResponse(Cmd, "{\"bindings\":" + bindingsJson + "}"));
}

void FWebServer::HandleLedgerPidCreate(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send)
{
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) return;
    const std::string pid = JsonGetString(DataStr, "pid");
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    std::string bindingJson, error;
    if (!m_LedgerManager.CreateLedgerPidBinding(username, pid, ledgerId, bindingJson, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to create PID" : error));
        return;
    }
    Send(BuildWSResponse(Cmd, "{\"binding\":" + bindingJson + "}"));
}

void FWebServer::HandleLedgerPidUpdateLedger(const std::string& DataStr,
                                             const std::string& Cmd, WS_SendFunc Send)
{
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) return;
    const int bindingId = JsonGetInt(DataStr, "id", 0);
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    std::string bindingJson, error;
    if (!m_LedgerManager.UpdateLedgerPidBindingLedger(username, bindingId, ledgerId, bindingJson, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to update PID ledger" : error));
        return;
    }
    Send(BuildWSResponse(Cmd, "{\"binding\":" + bindingJson + "}"));
}

void FWebServer::HandleLedgerPidDelete(const std::string& DataStr,
                                       const std::string& Cmd, WS_SendFunc Send)
{
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) return;
    const int bindingId = JsonGetInt(DataStr, "id", 0);
    std::string error;
    if (!m_LedgerManager.DeleteLedgerPidBinding(username, bindingId, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to delete PID" : error));
        return;
    }
    Send(BuildWSResponse(Cmd, "{\"ok\":true,\"id\":" + std::to_string(bindingId) + "}"));
}

void FWebServer::HandleLedgerUserProfile(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send)
{
    (void)DataStr;
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string outJson;
    if (m_LedgerManager.GetUserByUsername(username, outJson)) {
        Send(BuildWSResponse(Cmd, outJson));
    } else {
        Send(BuildWSError(Cmd, "Failed to query user profile"));
    }
}

// ============================================================================
// 记账 API: ledger.user.delete
// ============================================================================

void FWebServer::HandleLedgerUserDelete(const std::string& DataStr,
                                         const std::string& Cmd, WS_SendFunc Send)
{
    std::string actorUsername;
    if (!EnsureAdminForRequest(Cmd, Send, &actorUsername)) {
        return;
    }

    const std::string username = JsonGetString(DataStr, "username");
    const bool bHardDelete = JsonGetString(DataStr, "mode") == "hard";
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Missing username"));
        return;
    }

    LedgerManager::FUserAccountMutationResult result;
    if (!m_LedgerManager.DeleteUserAccount(actorUsername, username, bHardDelete, result)) {
        Send(BuildWSError(Cmd, result.ErrorCode + ": " + result.Message));
        return;
    }

    m_AuthUsers.erase(username);
    RemoveUserSessionsFromMemory(username);
    std::ostringstream data;
    data << "{\"ok\":true,\"mode\":\"" << (bHardDelete ? "hard" : "soft")
         << "\",\"deletedSessionCount\":" << result.DeletedSessionCount << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.invite.create
// ============================================================================

void FWebServer::HandleLedgerInviteCreate(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send)
{
    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    const std::string inviteEntry = JsonGetString(DataStr, "inviteEntry");
    const std::string inviteType = JsonGetString(DataStr, "inviteType");
    const bool bLedgerDirectoryEntry = inviteEntry == "ledger_directory";
    const bool bAdminUserManagementEntry = inviteEntry == "admin_user_management";
    if (!bLedgerDirectoryEntry && !bAdminUserManagementEntry) {
        Send(BuildWSError(Cmd, "INVITE_ENTRY_REQUIRED: 必须从账本目录或管理员用户管理入口创建邀请"));
        return;
    }
    if ((bLedgerDirectoryEntry && inviteType != "join_family")
        || (bAdminUserManagementEntry && inviteType != "independent_user")) {
        Send(BuildWSError(Cmd, "INVITE_SCOPE_MISMATCH: 邀请入口与邀请类型不匹配"));
        return;
    }

    const bool bAutoJoinFamily = bLedgerDirectoryEntry;
    if (bAdminUserManagementEntry && !CurrentRequestHasPermission("admin")) {
        Send(BuildWSError(Cmd, "ADMIN_REQUIRED: 只有系统管理员才能创建独立用户邀请"));
        return;
    }

    int familyId = 0;
    int defaultLedgerId = 0;
    std::string familyRole = "member";
    if (bLedgerDirectoryEntry) {
        const int sourceLedgerId = JsonGetInt(DataStr, "sourceLedgerId", 0);
        if (sourceLedgerId <= 0 || !m_LedgerManager.GetLedgerFamilyId(sourceLedgerId, familyId) || familyId <= 0) {
            Send(BuildWSError(Cmd, "INVITE_SCOPE_INVALID: 当前账本不存在或未绑定家庭"));
            return;
        }
        defaultLedgerId = JsonGetInt(DataStr, "defaultLedgerId", 0);
        if (defaultLedgerId != 0 && defaultLedgerId != sourceLedgerId) {
            Send(BuildWSError(Cmd, "INVITE_SCOPE_INVALID: 默认账本必须是当前账本或留空"));
            return;
        }
        const std::string requestedFamilyRole = JsonGetString(DataStr, "familyRole");
        familyRole = requestedFamilyRole.empty() ? "member" : requestedFamilyRole;
    }

    // 邀请码只由服务端生成，避免客户端碰撞后覆盖既有邀请作用域。
    static const char* kInviteAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static thread_local std::mt19937_64 inviteRng(
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&inviteRng));
    std::uniform_int_distribution<size_t> inviteCharDist(0, std::strlen(kInviteAlphabet) - 1);
    std::string code;
    std::string err;
    for (int attempt = 0; attempt < 16; ++attempt) {
        code.clear();
        for (int index = 0; index < 4; ++index) {
            code.push_back(kInviteAlphabet[inviteCharDist(inviteRng)]);
        }
        FLedgerInviteRegistration existingInvite;
        std::string queryError;
        if (!m_LedgerManager.GetValidInviteRegistration(code, existingInvite, queryError)) {
            break;
        }
        code.clear();
    }
    if (code.size() != 4) {
        Send(BuildWSError(Cmd, "INVITE_CODE_COLLISION: 无法生成唯一邀请码"));
        return;
    }

    const bool bAllowIndependentInvite = m_RuntimeSettings.GetBool(
        "registrationInvite.allowNonAdminIndependentUserInvite",
        true);
    const bool bCanCreateIndependentInvite = CurrentRequestHasPermission("admin");
    int expiresInSec = m_RuntimeSettings.GetInt("registrationInvite.expiresSec", 24 * 60 * 60);
    if (expiresInSec <= 0) expiresInSec = 24 * 60 * 60;
    const int64_t expiresAt = static_cast<int64_t>(std::time(nullptr)) + expiresInSec;
    if (!m_LedgerManager.SaveInviteRegistration(
            code,
            bAutoJoinFamily,
            familyId,
            defaultLedgerId,
            familyRole,
            username,
            bAllowIndependentInvite,
            expiresAt,
            err)) {
        Send(BuildWSError(Cmd, err.empty() ? "Failed to save invite" : err));
        return;
    }

    // 返回数据库最终保存的可信作用域，避免回显客户端尚未推导的 familyId=0。
    FLedgerInviteRegistration savedInvite;
    if (!m_LedgerManager.GetValidInviteRegistration(code, savedInvite, err)) {
        Send(BuildWSError(Cmd, err.empty() ? "Failed to reload saved invite" : err));
        return;
    }

    std::ostringstream data;
    data << "{\"ok\":true,\"code\":\"" << EscapeJsonString(savedInvite.Code) << "\","
         << "\"inviteType\":\"" << (savedInvite.bAutoJoinFamily ? "join_family" : "independent_user") << "\","
         << "\"autoJoinFamily\":" << (savedInvite.bAutoJoinFamily ? "true" : "false") << ","
         << "\"familyId\":" << savedInvite.FamilyId << ","
         << "\"defaultLedgerId\":" << savedInvite.DefaultLedgerId << ","
         << "\"familyRole\":\"" << EscapeJsonString(savedInvite.FamilyRole) << "\","
         << "\"canCreateIndependentInvite\":"
         << (bCanCreateIndependentInvite ? "true" : "false") << ","
         << "\"expiresAt\":" << savedInvite.ExpiresAt << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.invite.current
// ============================================================================

void FWebServer::HandleLedgerInviteCurrent(const std::string& DataStr,
                                           const std::string& Cmd, WS_SendFunc Send)
{
    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    const std::string inviteEntry = JsonGetString(DataStr, "inviteEntry");
    const std::string inviteType = JsonGetString(DataStr, "inviteType");
    const bool bLedgerDirectoryEntry = inviteEntry == "ledger_directory";
    const bool bAdminUserManagementEntry = inviteEntry == "admin_user_management";
    if (!bLedgerDirectoryEntry && !bAdminUserManagementEntry) {
        Send(BuildWSError(Cmd, "INVITE_ENTRY_REQUIRED: 必须从账本目录或管理员用户管理入口查询邀请"));
        return;
    }
    if ((bLedgerDirectoryEntry && inviteType != "join_family")
        || (bAdminUserManagementEntry && inviteType != "independent_user")) {
        Send(BuildWSError(Cmd, "INVITE_SCOPE_MISMATCH: 邀请入口与邀请类型不匹配"));
        return;
    }

    const bool bAutoJoinFamily = bLedgerDirectoryEntry;
    if (bAdminUserManagementEntry && !CurrentRequestHasPermission("admin")) {
        Send(BuildWSError(Cmd, "ADMIN_REQUIRED: 只有系统管理员才能查询独立用户邀请"));
        return;
    }

    int familyId = 0;
    int defaultLedgerId = 0;
    std::string familyRole = "member";
    if (bLedgerDirectoryEntry) {
        const int sourceLedgerId = JsonGetInt(DataStr, "sourceLedgerId", 0);
        if (sourceLedgerId <= 0 || !m_LedgerManager.GetLedgerFamilyId(sourceLedgerId, familyId) || familyId <= 0) {
            Send(BuildWSError(Cmd, "INVITE_SCOPE_INVALID: 当前账本不存在或未绑定家庭"));
            return;
        }
        defaultLedgerId = JsonGetInt(DataStr, "defaultLedgerId", 0);
        if (defaultLedgerId != 0 && defaultLedgerId != sourceLedgerId) {
            Send(BuildWSError(Cmd, "INVITE_SCOPE_INVALID: 默认账本必须是当前账本或留空"));
            return;
        }
        const std::string requestedFamilyRole = JsonGetString(DataStr, "familyRole");
        familyRole = requestedFamilyRole.empty() ? "member" : requestedFamilyRole;
    }
    const bool bCanCreateIndependentInvite = CurrentRequestHasPermission("admin");

    FLedgerInviteRegistration invite;
    std::string err;
    if (!m_LedgerManager.GetCurrentInviteRegistration(
            username,
            bAutoJoinFamily,
            familyId,
            defaultLedgerId,
            familyRole,
            invite,
            err)) {
        if (!err.empty()) {
            Send(BuildWSError(Cmd, err));
            return;
        }
        std::ostringstream emptyData;
        emptyData << "{\"ok\":true,\"hasInvite\":false,"
                  << "\"canCreateIndependentInvite\":"
                  << (bCanCreateIndependentInvite ? "true" : "false") << "}";
        Send(BuildWSResponse(Cmd, emptyData.str()));
        return;
    }

    std::ostringstream data;
    data << "{\"ok\":true,\"hasInvite\":true,"
         << "\"code\":\"" << EscapeJsonString(invite.Code) << "\","
         << "\"inviteType\":\"" << (invite.bAutoJoinFamily ? "join_family" : "independent_user") << "\","
         << "\"autoJoinFamily\":" << (invite.bAutoJoinFamily ? "true" : "false") << ","
         << "\"familyId\":" << invite.FamilyId << ","
         << "\"defaultLedgerId\":" << invite.DefaultLedgerId << ","
         << "\"familyRole\":\"" << EscapeJsonString(invite.FamilyRole) << "\","
         << "\"canCreateIndependentInvite\":"
         << (bCanCreateIndependentInvite ? "true" : "false") << ","
         << "\"expiresAt\":" << invite.ExpiresAt << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 账单导入 API: ledger.bill_import.status
// ============================================================================

void FWebServer::HandleLedgerBillImportStatus(const std::string& DataStr,
                                               const std::string& Cmd,
                                               WS_SendFunc Send)
{
    int taskId = JsonGetInt(DataStr, "taskId", 0);
    if (taskId <= 0) {
        Send(BuildWSError(Cmd, "Missing taskId"));
        return;
    }

    FBillImportTaskItem item;
    if (!m_LedgerManager.GetBillImportTaskStatus(taskId, item)) {
        Send(BuildWSError(Cmd, "Task not found"));
        return;
    }

    std::ostringstream data;
    data << "{\"taskId\":" << item.Id
         << ",\"status\":\"" << item.Status << "\""
         << ",\"totalRows\":" << item.TotalRows
         << ",\"importedRows\":" << item.ImportedRows
         << ",\"insertedRows\":" << item.InsertedRows
         << ",\"updatedRows\":" << item.UpdatedRows
         << ",\"skippedRows\":" << item.SkippedRows
         << ",\"sourceType\":\"" << EscapeJsonString(item.SourceType) << "\""
         << ",\"message\":\"" << EscapeJsonString(item.Message) << "\"";
    if (!item.ErrorMessage.empty()) {
        data << ",\"error\":\"" << EscapeJsonString(item.ErrorMessage) << "\"";
    }
    data << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.group.list
// ============================================================================

void FWebServer::HandleLedgerGroupList(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send)
{
    // 从 token 获取当前用户
    std::string username;
    GetCurrentRequestUsername(username);

    std::string outJson;
    int currentGroupId = 0;
    m_LedgerManager.GetCurrentGroupId(username, currentGroupId);
    if (m_LedgerManager.GetUserGroups(username, outJson)) {
        if (currentGroupId <= 0) {
            m_LedgerManager.GetCurrentGroupId(username, currentGroupId);
        }
        std::ostringstream data;
        data << "{\"currentGroupId\":" << currentGroupId
             << ",\"groups\":" << outJson << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, "Failed to query groups"));
    }
}

// ============================================================================
// 记账 API: ledger.group.create
// ============================================================================

void FWebServer::HandleLedgerGroupCreate(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send)
{
    std::string name = JsonGetString(DataStr, "name");

    if (name.empty()) {
        Send(BuildWSError(Cmd, "Missing group name"));
        return;
    }

    std::string username;
    GetCurrentRequestUsername(username);

    int groupId = 0;
    if (m_LedgerManager.CreateGroup(name, username, groupId)) {
        std::ostringstream data;
        data << "{\"id\":" << groupId << ",\"name\":\""
             << EscapeJsonString(name) << "\"}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, "Failed to create group"));
    }
}

// ============================================================================
// 记账 API: ledger.group.delete
// ============================================================================

void FWebServer::HandleLedgerGroupDelete(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send)
{
    int groupId = JsonGetInt(DataStr, "groupId", 0);
    if (groupId <= 0) {
        Send(BuildWSError(Cmd, "Missing groupId"));
        return;
    }

    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty() && m_bAuthEnabled) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    std::string error;
    if (m_LedgerManager.DeleteGroup(groupId, username, error)) {
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to delete ledger" : error));
    }
}

// ============================================================================
// 记账 API: ledger.group.set_current
// ============================================================================

void FWebServer::HandleLedgerGroupSetCurrent(const std::string& DataStr,
                                             const std::string& Cmd, WS_SendFunc Send)
{
    int groupId = JsonGetInt(DataStr, "groupId", 0);
    if (groupId <= 0) {
        Send(BuildWSError(Cmd, "Missing groupId"));
        return;
    }

    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty() && m_bAuthEnabled) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    std::string error;
    if (m_LedgerManager.SetCurrentGroupId(username, groupId, error)) {
        std::ostringstream data;
        data << "{\"ok\":true,\"currentGroupId\":" << groupId << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to set current group" : error));
    }
}

// ============================================================================
// 记账 API: ledger.group.members
// ============================================================================

void FWebServer::HandleLedgerGroupMembers(const std::string& DataStr,
                                           const std::string& Cmd, WS_SendFunc Send)
{
    int groupId = JsonGetInt(DataStr, "groupId", 0);
    if (groupId <= 0) {
        Send(BuildWSError(Cmd, "Missing groupId"));
        return;
    }
    if (!EnsureGroupMemberForRequest(groupId, Cmd, Send)) {
        return;
    }

    std::string outJson;
    if (m_LedgerManager.GetGroupMembers(groupId, outJson)) {
        std::ostringstream data;
        data << "{\"members\":" << outJson << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, "Failed to query members"));
    }
}

// ============================================================================
// 记账 API: ledger.group.add_member
// 兼容旧命令：禁止直接落库添加，改为创建待对方确认的邀请。
// ============================================================================

void FWebServer::HandleLedgerGroupAddMember(const std::string& DataStr,
                                             const std::string& Cmd, WS_SendFunc Send)
{
    HandleLedgerGroupInviteMember(DataStr, Cmd, Send);
}

// ============================================================================
// 记账 API: ledger.group.invite_member
// ============================================================================

void FWebServer::HandleLedgerGroupInviteMember(const std::string& DataStr,
                                               const std::string& Cmd, WS_SendFunc Send)
{
    int groupId = JsonGetInt(DataStr, "groupId", 0);
    std::string memberName = JsonGetString(DataStr, "username");
    std::string role = JsonGetString(DataStr, "role");
    if (role.empty()) role = "member";

    if (groupId <= 0 || memberName.empty()) {
        Send(BuildWSError(Cmd, "Missing groupId or username"));
        return;
    }

    std::string currentUsername;
    if (!EnsureGroupMemberForRequest(groupId, Cmd, Send, &currentUsername)) {
        return;
    }
    if (!m_LedgerManager.IsGroupAdmin(groupId, currentUsername)) {
        Send(BuildWSError(Cmd, "Only ledger owner or admin can invite member"));
        return;
    }

    // 系统参数已由 RuntimeSettingManager 维护为线程安全快照，新建邀请直接读取当前有效值。
    const int expiresInSec = m_RuntimeSettings.GetInt("ledger.memberInviteExpiresSec", 24 * 60 * 60);

    FLedgerMemberInvite invite;
    std::string error;
    if (!m_LedgerManager.CreateMemberInvite(groupId, memberName, role, currentUsername,
                                            expiresInSec, invite, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to create member invite" : error));
        return;
    }

    std::ostringstream data;
    data << "{\"ok\":true,"
         << "\"inviteId\":" << invite.Base.Id << ","
         << "\"status\":\"" << EscapeJsonString(invite.Base.Status) << "\","
         << "\"username\":\"" << EscapeJsonString(invite.Base.TargetUsername) << "\","
         << "\"groupId\":" << invite.GroupId << ","
         << "\"groupName\":\"" << EscapeJsonString(invite.GroupName) << "\","
         << "\"role\":\"" << EscapeJsonString(invite.Role) << "\","
         << "\"expiresAt\":" << invite.Base.ExpiresAt << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.group.invites.incoming
// ============================================================================

void FWebServer::HandleLedgerGroupIncomingInvites(const std::string& DataStr,
                                                  const std::string& Cmd, WS_SendFunc Send)
{
    (void)DataStr;
    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string outJson;
    std::string error;
    if (!m_LedgerManager.GetIncomingMemberInvites(username, outJson, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to query incoming invites" : error));
        return;
    }

    std::ostringstream data;
    data << "{\"invites\":" << outJson << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.group.invites.sent
// ============================================================================

void FWebServer::HandleLedgerGroupSentInvites(const std::string& DataStr,
                                              const std::string& Cmd, WS_SendFunc Send)
{
    int groupId = JsonGetInt(DataStr, "groupId", 0);
    if (groupId <= 0) {
        Send(BuildWSError(Cmd, "Missing groupId"));
        return;
    }

    std::string username;
    if (!EnsureGroupMemberForRequest(groupId, Cmd, Send, &username)) {
        return;
    }

    std::string outJson;
    std::string error;
    if (!m_LedgerManager.GetSentMemberInvites(groupId, username, outJson, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to query sent invites" : error));
        return;
    }

    std::ostringstream data;
    data << "{\"invites\":" << outJson << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.group.invite.accept
// ============================================================================

void FWebServer::HandleLedgerGroupAcceptInvite(const std::string& DataStr,
                                               const std::string& Cmd, WS_SendFunc Send)
{
    int inviteId = JsonGetInt(DataStr, "inviteId", 0);
    if (inviteId <= 0) {
        Send(BuildWSError(Cmd, "Missing inviteId"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    int groupId = 0;
    std::string error;
    if (!m_LedgerManager.AcceptMemberInvite(inviteId, username, groupId, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to accept invite" : error));
        return;
    }

    std::ostringstream data;
    data << "{\"ok\":true,\"groupId\":" << groupId << "}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.group.invite.reject
// ============================================================================

void FWebServer::HandleLedgerGroupRejectInvite(const std::string& DataStr,
                                               const std::string& Cmd, WS_SendFunc Send)
{
    int inviteId = JsonGetInt(DataStr, "inviteId", 0);
    if (inviteId <= 0) {
        Send(BuildWSError(Cmd, "Missing inviteId"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string error;
    if (!m_LedgerManager.RejectMemberInvite(inviteId, username, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to reject invite" : error));
        return;
    }
    Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
}

// ============================================================================
// 记账 API: ledger.group.invite.cancel
// ============================================================================

void FWebServer::HandleLedgerGroupCancelInvite(const std::string& DataStr,
                                               const std::string& Cmd, WS_SendFunc Send)
{
    int inviteId = JsonGetInt(DataStr, "inviteId", 0);
    if (inviteId <= 0) {
        Send(BuildWSError(Cmd, "Missing inviteId"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string error;
    if (!m_LedgerManager.CancelMemberInvite(inviteId, username, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to cancel invite" : error));
        return;
    }
    Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
}

// ============================================================================
// 记账 API: ledger.group.remove_member
// ============================================================================

void FWebServer::HandleLedgerGroupRemoveMember(const std::string& DataStr,
                                                const std::string& Cmd, WS_SendFunc Send)
{
    int groupId = JsonGetInt(DataStr, "groupId", 0);
    std::string memberName = JsonGetString(DataStr, "username");

    if (groupId <= 0 || memberName.empty()) {
        Send(BuildWSError(Cmd, "Missing groupId or username"));
        return;
    }
    std::string currentUsername;
    if (!EnsureGroupMemberForRequest(groupId, Cmd, Send, &currentUsername)) {
        return;
    }

    int familyId = 0;
    if (!m_LedgerManager.GetLedgerFamilyId(groupId, familyId)) {
        Send(BuildWSError(Cmd, "Ledger not found"));
        return;
    }

    // groupId 是兼容协议字段，其值为 V2 账本 ID；实际成员删除必须进入账本所属家庭领域。
    std::string error;
    if (m_LedgerManager.RemoveFamilyMember(familyId, memberName, currentUsername, error)) {
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to remove family member" : error));
    }
}

// ============================================================================
// 记账 API: ledger.category.list
// ============================================================================

void FWebServer::HandleLedgerCategoryList(const std::string& DataStr,
                                           const std::string& Cmd, WS_SendFunc Send)
{
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    const std::string type = JsonGetString(DataStr, "type");

    if (ledgerId <= 0) {
        Send(BuildWSError(Cmd, "Missing ledgerId"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string outJson;
    std::string error;
    if (m_LedgerManager.GetLedgerCategories(ledgerId,
                                             type,
                                             username,
                                             outJson,
                                             error)) {
        std::ostringstream data;
        data << "{\"categories\":" << outJson << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to query categories" : error));
    }
}

// ============================================================================
// 记账 API: ledger.category.create
// ============================================================================

void FWebServer::HandleLedgerCategoryCreate(const std::string& DataStr,
                                             const std::string& Cmd, WS_SendFunc Send)
{
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    const std::string name = JsonGetString(DataStr, "name");
    std::string type = JsonGetString(DataStr, "type");
    const int parentId = JsonGetInt(DataStr, "parentId", 0);
    const int sortOrder = JsonGetInt(DataStr, "sortOrder", 0);
    if (type.empty()) type = "expense";

    if (ledgerId <= 0 || name.empty()) {
        Send(BuildWSError(Cmd, "Missing ledgerId or name"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    int categoryId = 0;
    std::string error;
    if (m_LedgerManager.CreateLedgerCategory(ledgerId,
                                              name,
                                              type,
                                              parentId,
                                              sortOrder,
                                              username,
                                              categoryId,
                                              error)) {
        std::ostringstream data;
        data << "{\"id\":" << categoryId << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to create category" : error));
    }
}

// ============================================================================
// 记账 API: ledger.category.update
// ============================================================================

void FWebServer::HandleLedgerCategoryUpdate(const std::string& DataStr,
                                             const std::string& Cmd, WS_SendFunc Send)
{
    const int categoryId = JsonGetInt(DataStr, "id", 0);
    const std::string name = JsonGetString(DataStr, "name");
    const std::string type = JsonGetString(DataStr, "type");
    const int parentId = JsonGetInt(DataStr, "parentId", 0);
    const int sortOrder = JsonGetInt(DataStr, "sortOrder", 0);

    if (categoryId <= 0 || name.empty()) {
        Send(BuildWSError(Cmd, "Missing id or name"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string error;
    if (m_LedgerManager.UpdateLedgerCategory(categoryId,
                                              name,
                                              type,
                                              parentId,
                                              sortOrder,
                                              username,
                                              error)) {
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to update category" : error));
    }
}

// ============================================================================
// 记账 API: ledger.category.delete
// ============================================================================

void FWebServer::HandleLedgerCategoryDelete(const std::string& DataStr,
                                             const std::string& Cmd, WS_SendFunc Send)
{
    const int categoryId = JsonGetInt(DataStr, "id", 0);
    if (categoryId <= 0) {
        Send(BuildWSError(Cmd, "Missing id"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string error;
    if (m_LedgerManager.DeleteLedgerCategory(categoryId, username, error)) {
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to delete category" : error));
    }
}

// ============================================================================
// 记账 API: ledger.transaction.create
// ============================================================================

void FWebServer::HandleLedgerTransactionCreate(const std::string& DataStr,
                                                const std::string& Cmd, WS_SendFunc Send)
{
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    int categoryId = JsonGetInt(DataStr, "categoryId", 0);
    double amount = (double)JsonGetInt(DataStr, "amount", 0);
    std::string type = JsonGetString(DataStr, "type");
    std::string description = JsonGetString(DataStr, "description");
    std::string date = JsonGetString(DataStr, "date");

    // amount 可能是浮点数，从 DataStr 中直接解析
    {
        std::string search = "\"amount\"";
        size_t pos = DataStr.find(search);
        if (pos != std::string::npos) {
            pos = DataStr.find(':', pos + search.size());
            if (pos != std::string::npos) {
                pos++;
                while (pos < DataStr.size() && (DataStr[pos] == ' ' || DataStr[pos] == '\t'))
                    pos++;
                if (pos < DataStr.size()) {
                    char* end = nullptr;
                    amount = strtod(DataStr.c_str() + pos, &end);
                }
            }
        }
    }

    if (type.empty()) type = "expense";
    if (date.empty()) {
        // 默认今天
        time_t now = time(nullptr);
        struct tm tmInfo;
#ifdef _WIN32
        localtime_s(&tmInfo, &now);
#else
        localtime_r(&now, &tmInfo);
#endif
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tmInfo);
        date = buf;
    }

    if (ledgerId <= 0 || categoryId <= 0 || amount <= 0) {
        Send(BuildWSError(Cmd, "Missing or invalid ledgerId, categoryId, or amount"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    int transactionId = 0;
    std::string error;
    if (m_LedgerManager.CreateLedgerTransaction(ledgerId,
                                                 categoryId,
                                                 amount,
                                                 type,
                                                 description,
                                                 username,
                                                 date,
                                                 transactionId,
                                                 error)) {
        std::ostringstream data;
        data << "{\"id\":" << transactionId << ",\"ok\":true}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to add transaction" : error));
    }
}

// ============================================================================
// 记账 API: ledger.transaction.list
// ============================================================================

void FWebServer::HandleLedgerTransactionList(const std::string& DataStr,
                                              const std::string& Cmd, WS_SendFunc Send)
{
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    std::string dateFrom = JsonGetString(DataStr, "dateFrom");
    std::string dateTo = JsonGetString(DataStr, "dateTo");
    int categoryId = JsonGetInt(DataStr, "categoryId", 0);
    std::string type = JsonGetString(DataStr, "type");
    int offset = JsonGetInt(DataStr, "offset", 0);
    int limit = JsonGetInt(DataStr, "limit", 50);
    int parentCategoryId = JsonGetInt(DataStr, "parentCategoryId", 0);
    std::string sortBy = JsonGetString(DataStr, "sortBy");
    std::string sortOrder = JsonGetString(DataStr, "sortOrder");

    if (ledgerId <= 0) {
        Send(BuildWSError(Cmd, "Missing ledgerId"));
        return;
    }
    if (limit <= 0 || limit > 200) limit = 50;
    if (offset < 0) offset = 0;

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string outJson;
    std::string error;
    int total = 0;
    if (m_LedgerManager.GetLedgerTransactions(ledgerId,
                                               username,
                                               dateFrom,
                                               dateTo,
                                               categoryId,
                                               type,
                                               offset,
                                               limit,
                                               outJson,
                                               total,
                                               error,
                                               parentCategoryId,
                                               sortBy,
                                               sortOrder)) {
        std::ostringstream data;
        data << "{\"total\":" << total << ","
             << "\"transactions\":" << outJson << "}";
        Send(BuildWSResponse(Cmd, data.str()));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to query transactions" : error));
    }
}

// ============================================================================
// 记账 API: ledger.transaction.update
// ============================================================================

void FWebServer::HandleLedgerTransactionUpdate(const std::string& DataStr,
                                                const std::string& Cmd, WS_SendFunc Send)
{
    int id = JsonGetInt(DataStr, "id", 0);
    int categoryId = JsonGetInt(DataStr, "categoryId", 0);
    double amount = 0;
    std::string type = JsonGetString(DataStr, "type");
    std::string description = JsonGetString(DataStr, "description");
    std::string date = JsonGetString(DataStr, "date");

    // 解析 amount 浮点数
    {
        std::string search = "\"amount\"";
        size_t pos = DataStr.find(search);
        if (pos != std::string::npos) {
            pos = DataStr.find(':', pos + search.size());
            if (pos != std::string::npos) {
                pos++;
                while (pos < DataStr.size() && (DataStr[pos] == ' ' || DataStr[pos] == '\t'))
                    pos++;
                if (pos < DataStr.size()) {
                    char* end = nullptr;
                    amount = strtod(DataStr.c_str() + pos, &end);
                }
            }
        }
    }

    if (id <= 0) {
        Send(BuildWSError(Cmd, "Missing id"));
        return;
    }
    if (categoryId <= 0) {
        Send(BuildWSError(Cmd, "Missing categoryId"));
        return;
    }
    if (amount <= 0) {
        Send(BuildWSError(Cmd, "Amount must be greater than 0"));
        return;
    }
    if (type.empty()) type = "expense";
    if (type != "expense" && type != "income") {
        Send(BuildWSError(Cmd, "Invalid transaction type"));
        return;
    }
    if (date.empty()) {
        Send(BuildWSError(Cmd, "Missing transaction date"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string error;
    if (m_LedgerManager.UpdateLedgerTransaction(id,
                                                 categoryId,
                                                 amount,
                                                 type,
                                                 description,
                                                 date,
                                                 username,
                                                 error)) {
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to update transaction" : error));
    }
}

// ============================================================================
// 记账 API: ledger.transaction.delete
// ============================================================================

void FWebServer::HandleLedgerTransactionDelete(const std::string& DataStr,
                                                const std::string& Cmd, WS_SendFunc Send)
{
    int id = JsonGetInt(DataStr, "id", 0);

    if (id <= 0) {
        Send(BuildWSError(Cmd, "Missing id"));
        return;
    }

    std::string username;
    if (!EnsureUserForRequest(Cmd, Send, &username)) {
        return;
    }

    std::string error;
    if (m_LedgerManager.DeleteLedgerTransaction(id, username, error)) {
        Send(BuildWSResponse(Cmd, "{\"ok\":true}"));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to delete transaction" : error));
    }
}

// ============================================================================
// 记账 API: ledger.transaction.stats
// ============================================================================

void FWebServer::HandleLedgerTransactionStats(const std::string& DataStr,
                                               const std::string& Cmd, WS_SendFunc Send)
{
    const int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);
    const std::string dateFrom = JsonGetString(DataStr, "dateFrom");
    const std::string dateTo = JsonGetString(DataStr, "dateTo");
    const std::string groupBy = JsonGetString(DataStr, "groupBy");
    const int parentCategoryId = JsonGetInt(DataStr, "parentCategoryId", 0);

    if (ledgerId <= 0) {
        Send(BuildWSError(Cmd, "Missing ledgerId"));
        return;
    }

    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    // 授权统一下沉到账本领域层；当前 UI 上下文和请求中的 ledgerId 均不能单独构成权限依据。
    std::string outJson;
    std::string error;
    if (m_LedgerManager.GetLedgerStats(ledgerId,
                                       username,
                                       dateFrom,
                                       dateTo,
                                       groupBy,
                                       outJson,
                                       error,
                                       parentCategoryId)) {
        Send(BuildWSResponse(Cmd, outJson));
    } else {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to query ledger stats" : error));
    }
}

// ============================================================================
// 记账 API: ledger.voice.create — 语音记账入口
// 接收语音识别自然文本，异步解析后写入数据库
// ============================================================================

void FWebServer::HandleLedgerVoiceCreate(const std::string& DataStr,
                                          const std::string& Cmd, WS_SendFunc Send)
{
    // ---- 提取参数 ----
    const std::string text = JsonGetString(DataStr, "text");
    int ledgerId = JsonGetInt(DataStr, "ledgerId", 0);

    if (text.empty()) {
        Send(BuildWSError(Cmd, "Missing 'text' field"));
        return;
    }

    // ---- 获取当前用户名 ----
    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty()) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    if (ledgerId <= 0) {
        // 当前账本只提供缺省 UI 上下文；CreateVoiceInputTransaction 仍执行家庭成员授权。
        ledgerId = ResolveDefaultLedgerIdForUser(username);
        if (ledgerId <= 0) {
            Send(BuildWSError(Cmd, "Missing or invalid 'ledgerId'"));
            return;
        }
    }

    int transactionId = 0;
    std::string error;
    if (!m_LedgerManager.CreateVoiceInputTransaction(ledgerId, text, username,
                                                      transactionId, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to create voice transaction" : error));
        return;
    }

    std::ostringstream data;
    data << "{\"ok\":true,\"transactionId\":" << transactionId
         << ",\"message\":\"语音文本已录入，等待后台解析\"}";
    Send(BuildWSResponse(Cmd, data.str()));
}

// ============================================================================
// 记账 API: ledger.voice.test — 手动录入语音识别文本账目
// 创建 amount=0 的待解析语音流水，后台线程从数据库队列解析并回填
// ============================================================================

void FWebServer::HandleLedgerVoiceTest(const std::string& DataStr,
                                        const std::string& Cmd, WS_SendFunc Send)
{
    HandleLedgerVoiceCreate(DataStr, Cmd, Send);
}

// ============================================================================
// 记账 API: ledger.voice.parse — 手动补充解析 amount=0 的语音流水
// ============================================================================

void FWebServer::HandleLedgerVoiceParse(const std::string& DataStr,
                                         const std::string& Cmd, WS_SendFunc Send)
{
    int transactionId = JsonGetInt(DataStr, "id", 0);
    if (transactionId <= 0) {
        transactionId = JsonGetInt(DataStr, "transactionId", 0);
    }
    if (transactionId <= 0) {
        Send(BuildWSError(Cmd, "Missing transaction id"));
        return;
    }

    std::string username;
    GetCurrentRequestUsername(username);
    if (username.empty() && m_bAuthEnabled) {
        Send(BuildWSError(Cmd, "Authentication required"));
        return;
    }

    std::string error;
    if (!m_LedgerManager.RequeueVoiceInputTransaction(transactionId, username, error)) {
        Send(BuildWSError(Cmd, error.empty() ? "Failed to queue voice transaction" : error));
        return;
    }

    std::ostringstream data;
    data << "{\"ok\":true,\"transactionId\":" << transactionId
         << ",\"message\":\"已加入解析队列\"}";
    Send(BuildWSResponse(Cmd, data.str()));
}