// LedgerManager.cpp — 家庭记账数据库管理模块实现

#include "LedgerManager.h"
#include "JsonLite.h"
#include "SystemUtils.h"

extern "C" {
#include "sqlite3.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <vector>
#include <ctime>
#include <algorithm>

namespace fs = std::filesystem;
using namespace std;

// 安全获取 C 字符串指针，为 null 时返回默认值（替代 GNU ?: 扩展）
static inline const char* SafeCStr(const char* s, const char* defaultVal) {
    return s ? s : defaultVal;
}

static bool SqliteTableExists(sqlite3* db, const string& TableName)
{
    if (!db || TableName.empty()) return false;

    bool exists = false;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, TableName.c_str(), -1, SQLITE_TRANSIENT);
        exists = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    return exists;
}

static bool SqliteColumnExists(sqlite3* db, const string& TableName, const string& ColumnName)
{
    bool exists = false;
    string sql = "PRAGMA table_info(" + TableName + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName && ColumnName == colName) {
                exists = true;
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    return exists;
}

static string SqliteScalarText(sqlite3* db, const string& Sql)
{
    string result;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, Sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* text = (const char*)sqlite3_column_text(stmt, 0);
            if (text) result = text;
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

// ============================================================================
// JSON 极简工具（与 WebServer.cpp 中的实现风格一致）
// ============================================================================

string LedgerManager::EscapeJsonString(const string& S)
{
    return JsonLite::EscapeString(S);
}

// 从 JSON 对象中提取字符串值
static string JsonGetString(const string& Json, const string& Key)
{
    return JsonLite::GetStringOrDefault(Json, Key);
}

// 从 JSON 对象中提取 JSON 数组的原始内容（不含外层方括号）
static string JsonGetArrayRaw(const string& Json, const string& Key)
{
    return JsonLite::GetArrayRaw(Json, Key);
}

// 把 JSON 数组拆分到 object 字符串列表
static vector<string> JsonSplitArrayObjects(const string& ArrayRaw)
{
    return JsonLite::SplitTopLevelObjects(ArrayRaw);
}

static bool PermissionArrayContains(const string& PermissionsJson,
                                    const string& PermissionName)
{
    if (PermissionsJson.empty() || PermissionName.empty()) return false;
    return PermissionsJson.find("\"" + PermissionName + "\"") != string::npos;
}

static string NormalizePermissionsJson(const string& PermissionsJson,
                                       bool bEnsureUserPermission = true)
{
    vector<string> normalizedPermissions;
    auto appendPermission = [&](const string& permission) {
        if (permission.empty()) return;
        if (find(normalizedPermissions.begin(), normalizedPermissions.end(), permission)
            != normalizedPermissions.end()) {
            return;
        }
        normalizedPermissions.push_back(permission);
    };

    string permsRaw = PermissionsJson;
    size_t leftBracket = permsRaw.find('[');
    size_t rightBracket = permsRaw.rfind(']');
    if (leftBracket != string::npos && rightBracket != string::npos && rightBracket > leftBracket) {
        permsRaw = permsRaw.substr(leftBracket + 1, rightBracket - leftBracket - 1);
    }

    size_t pos = 0;
    while (true) {
        size_t start = permsRaw.find('"', pos);
        if (start == string::npos) break;
        size_t end = permsRaw.find('"', start + 1);
        if (end == string::npos) break;
        appendPermission(permsRaw.substr(start + 1, end - start - 1));
        pos = end + 1;
    }

    if (bEnsureUserPermission) {
        bool hasUserPermission = false;
        bool hasViewerPermission = false;
        for (const auto& permission : normalizedPermissions) {
            if (permission == "user") hasUserPermission = true;
            if (permission == "viewer") hasViewerPermission = true;
        }
        if (!hasUserPermission && hasViewerPermission) {
            appendPermission("user");
        }
        if (!hasUserPermission && !hasViewerPermission) {
            appendPermission("user");
        }
    }

    ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < normalizedPermissions.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << JsonLite::EscapeString(normalizedPermissions[i]) << "\"";
    }
    oss << "]";
    return oss.str();
}

// ============================================================================
// 构造 / 析构
// ============================================================================

LedgerManager::LedgerManager()
{
}

LedgerManager::~LedgerManager()
{
    Shutdown();
}

// ============================================================================
// 初始化 / 关闭
// ============================================================================

bool LedgerManager::Initialize(const string& DbPath)
{
    lock_guard<mutex> lock(m_Mutex);

    int rc = sqlite3_open(DbPath.c_str(), &m_Db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[Ledger] Cannot open database: %s\n", sqlite3_errmsg(m_Db));
        sqlite3_close(m_Db);
        m_Db = nullptr;
        return false;
    }

    // 启用 WAL 模式提高并发读取性能
    ExecuteSql("PRAGMA journal_mode=WAL;");
    ExecuteSql("PRAGMA foreign_keys=ON;");

    if (!CreateTables()) {
        fprintf(stderr, "[Ledger] Failed to create tables\n");
        sqlite3_close(m_Db);
        m_Db = nullptr;
        return false;
    }

    if (!EnsureDefaultSystemSettings()) {
        fprintf(stderr, "[Ledger] Failed to initialize default system settings\n");
        sqlite3_close(m_Db);
        m_Db = nullptr;
        return false;
    }

    printf("[Ledger] Database initialized: %s\n", DbPath.c_str());
    return true;
}

void LedgerManager::Shutdown()
{
    lock_guard<mutex> lock(m_Mutex);
    if (m_Db) {
        sqlite3_close(m_Db);
        m_Db = nullptr;
        printf("[Ledger] Database closed\n");
    }
}

// ============================================================================
// 建表
// ============================================================================

bool LedgerManager::CreateTables()
{
    // 新领域模型不迁移旧业务数据。检测到旧表时明确拒绝启动，避免普通启动流程静默破坏数据库。
    if (SqliteTableExists(m_Db, "family_groups")
        || SqliteTableExists(m_Db, "group_members")) {
        fprintf(stderr,
                "[Ledger] Incompatible legacy database schema detected. "
                "Back up and explicitly remove the old database before starting this version.\n");
        return false;
    }

    const char* kSchema = R"SQL(
        BEGIN IMMEDIATE TRANSACTION;

        CREATE TABLE IF NOT EXISTS schema_metadata (
            schema_key TEXT PRIMARY KEY,
            schema_value TEXT NOT NULL
        );
        INSERT INTO schema_metadata (schema_key, schema_value)
        VALUES ('schema_version', '3')
        ON CONFLICT(schema_key) DO NOTHING;

        -- 用户上下文不构成授权；访问权只由 family_members 决定。
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL DEFAULT '',
            permissions TEXT NOT NULL DEFAULT '["user"]',
            pid TEXT NOT NULL DEFAULT '',
            current_family_id INTEGER DEFAULT NULL,
            current_ledger_id INTEGER DEFAULT NULL,
            is_active INTEGER NOT NULL DEFAULT 1,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            last_login TEXT DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);
        CREATE INDEX IF NOT EXISTS idx_users_pid ON users(pid);
        CREATE INDEX IF NOT EXISTS idx_users_current_family ON users(current_family_id);
        CREATE INDEX IF NOT EXISTS idx_users_current_ledger ON users(current_ledger_id);

        CREATE TABLE IF NOT EXISTS auth_sessions (
            token TEXT PRIMARY KEY,
            username TEXT NOT NULL REFERENCES users(username) ON DELETE CASCADE,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        );
        CREATE INDEX IF NOT EXISTS idx_auth_sessions_username_created
            ON auth_sessions(username, created_at);

        -- 家庭是成员关系和业务授权边界。
        CREATE TABLE IF NOT EXISTS families (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            created_by INTEGER NOT NULL REFERENCES users(id),
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );

        CREATE TABLE IF NOT EXISTS family_members (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            family_id INTEGER NOT NULL REFERENCES families(id) ON DELETE CASCADE,
            user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            role TEXT NOT NULL DEFAULT 'member' CHECK(role IN ('owner','member')),
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            UNIQUE(family_id, user_id)
        );
        CREATE INDEX IF NOT EXISTS idx_family_members_user ON family_members(user_id);
        CREATE INDEX IF NOT EXISTS idx_family_members_family_role
            ON family_members(family_id, role);

        -- 账本必须属于家庭；家庭成员天然访问家庭下全部账本。
        CREATE TABLE IF NOT EXISTS ledgers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            family_id INTEGER NOT NULL REFERENCES families(id) ON DELETE CASCADE,
            name TEXT NOT NULL,
            created_by INTEGER NOT NULL REFERENCES users(id),
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            UNIQUE(family_id, name)
        );
        CREATE INDEX IF NOT EXISTS idx_ledgers_family ON ledgers(family_id, id);

        -- PID 是独立的外部数据通道资源；一个用户可创建多个 PID，且每个 PID 必须定向绑定一个账本。
        CREATE TABLE IF NOT EXISTS ledger_pid_bindings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            pid TEXT NOT NULL UNIQUE,
            ledger_id INTEGER NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
            created_by INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE INDEX IF NOT EXISTS idx_ledger_pid_bindings_creator
            ON ledger_pid_bindings(created_by, id);
        CREATE INDEX IF NOT EXISTS idx_ledger_pid_bindings_ledger
            ON ledger_pid_bindings(ledger_id, id);

        CREATE TABLE IF NOT EXISTS account_categories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ledger_id INTEGER NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
            parent_id INTEGER NOT NULL DEFAULT 0,
            name TEXT NOT NULL,
            type TEXT NOT NULL DEFAULT 'expense' CHECK(type IN ('expense','income')),
            sort_order INTEGER NOT NULL DEFAULT 0,
            is_system INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE INDEX IF NOT EXISTS idx_categories_ledger
            ON account_categories(ledger_id, type, parent_id, sort_order, id);

        CREATE TABLE IF NOT EXISTS transactions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ledger_id INTEGER NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
            category_id INTEGER NOT NULL REFERENCES account_categories(id),
            amount REAL NOT NULL DEFAULT 0 CHECK(amount >= 0),
            type TEXT NOT NULL DEFAULT 'expense' CHECK(type IN ('expense','income')),
            description TEXT NOT NULL DEFAULT '',
            created_by INTEGER NOT NULL REFERENCES users(id),
            transaction_date TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            is_voice_input INTEGER NOT NULL DEFAULT 0,
            voice_text TEXT NOT NULL DEFAULT '',
            voice_parse_status TEXT NOT NULL DEFAULT ''
                CHECK(voice_parse_status IN ('','pending','processing','done','failed')),
            voice_parse_error TEXT NOT NULL DEFAULT '',
            voice_parsed_at TEXT NOT NULL DEFAULT '',
            voice_parse_completed INTEGER NOT NULL DEFAULT 0,
            import_source TEXT NOT NULL DEFAULT '',
            import_source_key TEXT NOT NULL DEFAULT '',
            import_raw_text TEXT NOT NULL DEFAULT '',
            imported_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_transactions_ledger_date
            ON transactions(ledger_id, transaction_date, id);
        CREATE INDEX IF NOT EXISTS idx_transactions_category ON transactions(category_id);
        CREATE INDEX IF NOT EXISTS idx_transactions_creator ON transactions(created_by, id);
        CREATE INDEX IF NOT EXISTS idx_transactions_voice_status
            ON transactions(is_voice_input, voice_parse_status, id);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_transactions_import_unique
            ON transactions(ledger_id, import_source, import_source_key)
            WHERE import_source <> '' AND import_source_key <> '';

        -- 注册邀请既支持独立用户，也支持注册后原子加入家庭。
        CREATE TABLE IF NOT EXISTS invite_registrations (
            code TEXT PRIMARY KEY,
            auto_join_family INTEGER NOT NULL DEFAULT 1 CHECK(auto_join_family IN (0,1)),
            family_id INTEGER DEFAULT NULL REFERENCES families(id) ON DELETE CASCADE,
            default_ledger_id INTEGER DEFAULT NULL REFERENCES ledgers(id) ON DELETE SET NULL,
            family_role TEXT NOT NULL DEFAULT 'member' CHECK(family_role IN ('owner','member')),
            created_by_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            expires_at INTEGER NOT NULL,
            CHECK((auto_join_family=0 AND family_id IS NULL AND default_ledger_id IS NULL)
               OR (auto_join_family=1 AND family_id IS NOT NULL))
        );
        CREATE INDEX IF NOT EXISTS idx_invite_registrations_creator
            ON invite_registrations(created_by_user_id, expires_at);
        CREATE INDEX IF NOT EXISTS idx_invite_registrations_expires
            ON invite_registrations(expires_at);

        CREATE TABLE IF NOT EXISTS pending_actions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            action_type TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending'
                CHECK(status IN ('pending','accepted','rejected','expired','cancelled')),
            created_by_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_by_username TEXT NOT NULL DEFAULT '',
            target_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            target_username TEXT NOT NULL DEFAULT '',
            family_id INTEGER DEFAULT NULL REFERENCES families(id) ON DELETE CASCADE,
            role TEXT NOT NULL DEFAULT '',
            payload_json TEXT NOT NULL DEFAULT '{}',
            created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            expires_at INTEGER NOT NULL DEFAULT 0,
            responded_at INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_pending_actions_target
            ON pending_actions(target_user_id, action_type, status, expires_at);
        CREATE INDEX IF NOT EXISTS idx_pending_actions_creator
            ON pending_actions(created_by_user_id, action_type, status, expires_at);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_pending_family_member_invites_unique
            ON pending_actions(action_type, target_user_id, family_id)
            WHERE action_type='family_member_invite' AND status='pending';

        CREATE TABLE IF NOT EXISTS bill_import_tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ledger_id INTEGER NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
            username TEXT NOT NULL,
            filename TEXT NOT NULL,
            file_content BLOB,
            status TEXT NOT NULL DEFAULT 'pending'
                CHECK(status IN ('pending','processing','done','failed')),
            total_rows INTEGER NOT NULL DEFAULT 0,
            imported_rows INTEGER NOT NULL DEFAULT 0,
            inserted_rows INTEGER NOT NULL DEFAULT 0,
            updated_rows INTEGER NOT NULL DEFAULT 0,
            skipped_rows INTEGER NOT NULL DEFAULT 0,
            source_type TEXT NOT NULL DEFAULT '',
            message TEXT NOT NULL DEFAULT '',
            error_message TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_bill_import_status
            ON bill_import_tasks(status, id);
        CREATE INDEX IF NOT EXISTS idx_bill_import_ledger
            ON bill_import_tasks(ledger_id, id);

        CREATE TABLE IF NOT EXISTS system_settings (
            setting_key TEXT PRIMARY KEY,
            setting_value TEXT NOT NULL DEFAULT '',
            value_type TEXT NOT NULL DEFAULT 'string',
            category TEXT NOT NULL DEFAULT 'general',
            is_sensitive INTEGER NOT NULL DEFAULT 0,
            description TEXT NOT NULL DEFAULT '',
            updated_by TEXT NOT NULL DEFAULT 'system',
            updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        );
        CREATE INDEX IF NOT EXISTS idx_system_settings_category
            ON system_settings(category, setting_key);

        COMMIT;
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_Db, kSchema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[Ledger] CreateTables error: %s\n", errMsg ? errMsg : "unknown");
        sqlite3_free(errMsg);
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    string schemaVersion = SqliteScalarText(
        m_Db,
        "SELECT schema_value FROM schema_metadata WHERE schema_key='schema_version';");
    if (schemaVersion == "2") {
        // V2 的 users.pid 迁移到独立绑定表。只有当前账本仍由真实家庭成员关系授权时才迁移，禁止静默回退到其他账本。
        const char* kMigrateV2ToV3 = R"SQL(
            BEGIN IMMEDIATE TRANSACTION;
            INSERT INTO ledger_pid_bindings(pid, ledger_id, created_by)
            SELECT u.pid, u.current_ledger_id, u.id
            FROM users u
            JOIN ledgers l ON l.id=u.current_ledger_id
            JOIN family_members fm ON fm.family_id=l.family_id AND fm.user_id=u.id
            WHERE u.pid<>'' AND u.is_active=1;
            UPDATE schema_metadata SET schema_value='3' WHERE schema_key='schema_version';
            COMMIT;
        )SQL";
        errMsg = nullptr;
        rc = sqlite3_exec(m_Db, kMigrateV2ToV3, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "[Ledger] V2 to V3 migration failed: %s\n", errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
            sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        schemaVersion = "3";
    }
    if (schemaVersion != "3") {
        fprintf(stderr,
                "[Ledger] Unsupported database schema version '%s'; expected version 3.\n",
                schemaVersion.c_str());
        return false;
    }

    return true;
}

// ============================================================================
// 创建默认分类
// ============================================================================

bool LedgerManager::CreateDefaultCategories(int LedgerId)
{
    if (!m_Db || LedgerId <= 0) return false;

    struct FDefaultCategory
    {
        const char* Name;
        const char* Type;
        int SortOrder;
    };

    static const FDefaultCategory kDefaults[] = {
        {"餐饮",     "expense",  1},
        {"交通",     "expense",  2},
        {"住房",     "expense",  3},
        {"日常用品", "expense",  4},
        {"养育赡养", "expense",  5},
        {"通讯数码", "expense",  6},
        {"医疗",     "expense",  7},
        {"教育",     "expense",  8},
        {"其他支出", "expense", 99},
        {"工资",     "income",   1},
        {"其他收入", "income",   2},
    };

    const char* sql =
        "INSERT INTO account_categories "
        "(ledger_id, parent_id, name, type, sort_order, is_system) "
        "VALUES (?, 0, ?, ?, ?, 1);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bool bSucceeded = true;
    for (const FDefaultCategory& item : kDefaults) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, LedgerId);
        sqlite3_bind_text(stmt, 2, item.Name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, item.Type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, item.SortOrder);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            bSucceeded = false;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return bSucceeded;
}

bool LedgerManager::IsProtectedFallbackCategoryName(const string& Name,
                                                    const string& Type) const
{
    return (Type == "expense" && Name == "其他支出")
        || (Type == "income" && Name == "其他收入");
}

// ============================================================================
// 会话管理
// ============================================================================

bool LedgerManager::LoadAuthSessions(vector<FLedgerAuthSession>& OutSessions)
{
    lock_guard<mutex> lock(m_Mutex);
    OutSessions.clear();
    if (!m_Db) return false;

    const char* sql =
        "SELECT s.token, s.username, s.created_at "
        "FROM auth_sessions s "
        "JOIN users u ON u.username=s.username "
        "WHERE u.is_active=1 "
        "ORDER BY s.created_at ASC, s.token ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] LoadAuthSessions prepare: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FLedgerAuthSession session;
        const char* token = (const char*)sqlite3_column_text(stmt, 0);
        const char* username = (const char*)sqlite3_column_text(stmt, 1);
        session.Token = SafeCStr(token, "");
        session.Username = SafeCStr(username, "");
        session.CreatedAt = sqlite3_column_int64(stmt, 2);
        if (!session.Token.empty() && !session.Username.empty()) {
            OutSessions.push_back(session);
        }
    }

    int rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[Ledger] LoadAuthSessions finalize: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    return true;
}

bool LedgerManager::UpsertAuthSession(const string& Token,
                                      const string& Username,
                                      int64_t CreatedAt)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || Token.empty() || Username.empty()) return false;

    const char* sql =
        "INSERT INTO auth_sessions (token, username, created_at) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(token) DO UPDATE SET "
        "username=excluded.username, created_at=excluded.created_at;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] UpsertAuthSession prepare: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }

    sqlite3_bind_text(stmt, 1, Token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, Username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(CreatedAt));

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[Ledger] UpsertAuthSession step: %s\n", sqlite3_errmsg(m_Db));
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool LedgerManager::DeleteAuthSession(const string& Token)
{
    if (Token.empty()) return true;
    vector<string> tokens;
    tokens.push_back(Token);
    return DeleteAuthSessionsByTokens(tokens);
}

bool LedgerManager::DeleteAuthSessionsByTokens(const vector<string>& Tokens)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;
    if (Tokens.empty()) return true;

    const char* sql = "DELETE FROM auth_sessions WHERE token=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] DeleteAuthSessionsByTokens prepare: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }

    bool ok = true;
    for (const auto& token : Tokens) {
        if (token.empty()) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "[Ledger] DeleteAuthSessionsByTokens step: %s\n", sqlite3_errmsg(m_Db));
            ok = false;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return ok;
}

// ============================================================================
// 安全执行 SQL（无回调）
// ============================================================================

bool LedgerManager::ExecuteSql(const string& Sql)
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_Db, Sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[Ledger] SQL error: %s\nSQL: %s\n",
                errMsg ? errMsg : "unknown", Sql.c_str());
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// ============================================================================
// 用户查询回调辅助 — 返回单行结果
// ============================================================================

// 通用查询: 执行 SQL，通过回调函数处理每行结果
// 返回行数，出错返回 -1
static int QueryRows(sqlite3* db, const string& Sql,
                     function<void(sqlite3_stmt*)> RowHandler)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, Sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] QueryRows prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RowHandler(stmt);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

// 获取单列字符串值
static string QueryString(sqlite3* db, const string& Sql)
{
    string result;
    QueryRows(db, Sql, [&](sqlite3_stmt* stmt) {
        const char* s = (const char*)sqlite3_column_text(stmt, 0);
        if (s) result = s;
    });
    return result;
}

// 调用者已持有 LedgerManager::m_Mutex 时使用，避免同一线程重复锁定 std::mutex。
static int GetUserIdNoLock(sqlite3* db, const string& Username)
{
    if (!db) return -1;

    int id = -1;
    const char* sql = "SELECT id FROM users WHERE username=? AND is_active=1;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    return id;
}

// 查询家庭角色。空字符串表示用户不是该家庭成员，或查询失败。
static string GetFamilyRoleNoLock(sqlite3* db, int FamilyId, int UserId)
{
    if (!db || FamilyId <= 0 || UserId <= 0) return "";

    const char* sql =
        "SELECT role FROM family_members WHERE family_id=? AND user_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";

    sqlite3_bind_int(stmt, 1, FamilyId);
    sqlite3_bind_int(stmt, 2, UserId);

    string role;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        role = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "");
    }
    sqlite3_finalize(stmt);
    return role;
}

// 查询账本所属家庭。返回值 <=0 表示账本不存在或查询失败。
static int GetLedgerFamilyIdNoLock(sqlite3* db, int LedgerId)
{
    if (!db || LedgerId <= 0) return 0;

    const char* sql = "SELECT family_id FROM ledgers WHERE id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    sqlite3_bind_int(stmt, 1, LedgerId);
    int familyId = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        familyId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return familyId;
}

// 家庭成员天然继承家庭下全部账本访问权，不建立账本级成员关系。
static bool CanAccessLedgerNoLock(sqlite3* db, int LedgerId, int UserId)
{
    const int familyId = GetLedgerFamilyIdNoLock(db, LedgerId);
    return familyId > 0 && !GetFamilyRoleNoLock(db, familyId, UserId).empty();
}

// 账本及分类管理权限只授予家庭 owner。
static bool CanManageLedgerNoLock(sqlite3* db, int LedgerId, int UserId)
{
    const int familyId = GetLedgerFamilyIdNoLock(db, LedgerId);
    return familyId > 0 && GetFamilyRoleNoLock(db, familyId, UserId) == "owner";
}

// 家庭角色只允许 owner/member；空值和其他非法值均不做隐式放宽。
static bool IsValidFamilyRole(const string& Role)
{
    return Role == "owner" || Role == "member";
}

// ============================================================================
// 系统参数默认值初始化
// ============================================================================

bool LedgerManager::EnsureDefaultSystemSettings()
{
    if (!m_Db) return false;

    vector<FSystemSettingItem> items;
    auto addSetting = [&](const string& key,
                          const string& value,
                          const string& valueType,
                          const string& category,
                          bool sensitive,
                          const string& description) {
        FSystemSettingItem item;
        item.SettingKey = key;
        item.SettingValue = value;
        item.ValueType = valueType;
        item.Category = category;
        item.bSensitive = sensitive;
        item.Description = description;
        items.push_back(item);
    };

    addSetting("server.listenIp",
               "0.0.0.0",
               "string", "server", false,
               "HTTP 服务监听地址；修改后需重启服务生效。默认 0.0.0.0。");
    addSetting("server.httpPort",
               "5080",
               "int", "server", false,
               "HTTP 服务监听端口；修改后需重启服务生效。默认 5080。");
    addSetting("server.wwwDir",
               "./www",
               "string", "server", false,
               "Web 前端静态资源目录。修改后新请求立即按新目录读取；建议填写绝对路径或可稳定解析的相对路径。");

    addSetting("auth.maxSessionsPerUser",
               "5",
               "int", "auth", false,
               "每个用户最多保留的登录会话数，<=0 表示不限制。");

    addSetting("ledger.memberInviteExpiresSec",
               "86400",
               "int", "ledger", false,
               "指定账号成员邀请有效期秒数。默认 86400，即 24 小时。接受或拒绝后邀请立即失效。已过期邀请会被标记为 expired。");

    addSetting("registrationInvite.expiresSec",
               "86400",
               "int", "registrationInvite", false,
               "注册邀请有效期秒数。默认 86400，即 24 小时；保存后新创建邀请立即使用，已创建邀请不追溯修改。");

    addSetting("registrationInvite.allowNonAdminIndependentUserInvite",
               "false",
               "bool", "registrationInvite", false,
               "兼容参数：是否允许非系统管理员的家庭 owner 创建独立用户注册邀请。当前产品入口仅向系统管理员开放。");

    addSetting("voiceLedger.enabled",
               "true",
               "bool", "voiceLedger", false,
               "是否启用语音记账能力。");
    addSetting("voiceLedger.apiKey",
               "",
               "string", "voiceLedger", true,
               "DeepSeek API Key，可为空。为空时语音记账能力初始化为不可用。");
    addSetting("voiceLedger.endpoint",
               "https://api.deepseek.com/chat/completions",
               "string", "voiceLedger", false,
               "语音记账模型调用地址。");
    addSetting("voiceLedger.model",
               "deepseek-chat",
               "string", "voiceLedger", false,
               "语音记账模型名称。");
    addSetting("voiceLedger.curlPath",
               "curl",
               "string", "voiceLedger", false,
               "调用外部 curl 的可执行路径。");
    addSetting("voiceLedger.timeoutSec",
               "60",
               "int", "voiceLedger", false,
               "语音记账请求超时秒数。");
    addSetting("voiceLedger.temperature",
               "0.1",
               "double", "voiceLedger", false,
               "语音记账模型 temperature 参数。");
    addSetting("voiceLedger.maxTokens",
               "512",
               "int", "voiceLedger", false,
               "语音记账模型最大 token 数。");

    const char* sql =
        "INSERT INTO system_settings (setting_key, setting_value, value_type, category, is_sensitive, description, updated_by, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now')) "
        "ON CONFLICT(setting_key) DO UPDATE SET "
        "value_type=excluded.value_type, category=excluded.category, is_sensitive=excluded.is_sensitive, "
        "description=excluded.description;";

    for (const auto& item : items) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            fprintf(stderr, "[Ledger] EnsureDefaultSystemSettings prepare: %s\n", sqlite3_errmsg(m_Db));
            return false;
        }
        sqlite3_bind_text(stmt, 1, item.SettingKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, item.SettingValue.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, item.ValueType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, item.Category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, item.bSensitive ? 1 : 0);
        sqlite3_bind_text(stmt, 6, item.Description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, "system", -1, SQLITE_TRANSIENT);
        int stepRc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (stepRc != SQLITE_DONE) {
            fprintf(stderr, "[Ledger] EnsureDefaultSystemSettings step failed for key '%s': %s\n",
                    item.SettingKey.c_str(), sqlite3_errmsg(m_Db));
            return false;
        }
    }

    return true;
}

bool LedgerManager::SerializeSystemSettingsToJson(std::string& OutJson,
                                                  const std::vector<FSystemSettingItem>& Items) const
{
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < Items.size(); ++i) {
        const auto& item = Items[i];
        // 敏感配置只能在服务端序列化边界脱敏，数据库与运行时快照始终保留真实值。
        const std::string displayValue = item.bSensitive
            ? SystemUtils::MaskSensitiveValue(item.SettingValue)
            : item.SettingValue;
        if (i > 0) oss << ",";
        oss << "{"
            << "\"settingKey\":\"" << EscapeJsonString(item.SettingKey) << "\","
            << "\"settingValue\":\"" << EscapeJsonString(displayValue) << "\","
            << "\"valueType\":\"" << EscapeJsonString(item.ValueType) << "\","
            << "\"category\":\"" << EscapeJsonString(item.Category) << "\","
            << "\"isSensitive\":" << (item.bSensitive ? "true" : "false") << ","
            << "\"description\":\"" << EscapeJsonString(item.Description) << "\","
            << "\"updatedBy\":\"" << EscapeJsonString(item.UpdatedBy) << "\","
            << "\"updatedAt\":\"" << EscapeJsonString(item.UpdatedAt) << "\""
            << "}";
    }
    oss << "]";
    OutJson = oss.str();
    return true;
}

bool LedgerManager::LoadSystemSettingRows(std::vector<FSystemSettingItem>& OutItems)
{
    OutItems.clear();
    if (!m_Db) return false;

    const char* sql =
        "SELECT setting_key, setting_value, value_type, category, is_sensitive, description, updated_by, "
        "datetime(updated_at, 'unixepoch', 'localtime') "
        "FROM system_settings "
        "WHERE setting_key NOT IN ('server.wsPort', 'server.enableWebSocket') "
        "ORDER BY category ASC, setting_key ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] LoadSystemSettingRows prepare: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FSystemSettingItem item;
        item.SettingKey = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "");
        item.SettingValue = SafeCStr((const char*)sqlite3_column_text(stmt, 1), "");
        item.ValueType = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "string");
        item.Category = SafeCStr((const char*)sqlite3_column_text(stmt, 3), "general");
        item.bSensitive = sqlite3_column_int(stmt, 4) != 0;
        item.Description = SafeCStr((const char*)sqlite3_column_text(stmt, 5), "");
        item.UpdatedBy = SafeCStr((const char*)sqlite3_column_text(stmt, 6), "system");
        item.UpdatedAt = SafeCStr((const char*)sqlite3_column_text(stmt, 7), "");
        OutItems.push_back(item);
    }

    sqlite3_finalize(stmt);
    return true;
}


bool LedgerManager::GetSystemSettings(std::string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    vector<FSystemSettingItem> items;
    if (!LoadSystemSettingRows(items)) return false;
    return SerializeSystemSettingsToJson(OutJson, items);
}

bool LedgerManager::GetSystemSettingValue(const std::string& SettingKey,
                                          std::string& OutValue)
{
    lock_guard<mutex> lock(m_Mutex);
    OutValue.clear();
    if (!m_Db || SettingKey.empty()) return false;

    const char* sql = "SELECT setting_value FROM system_settings WHERE setting_key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, SettingKey.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OutValue = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "");
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool LedgerManager::UpsertSystemSetting(const FSystemSettingItem& Setting,
                                        const std::string& UpdatedBy,
                                        FSystemSettingChangedEvent* OutEvent)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || Setting.SettingKey.empty()) return false;

    string oldValue;
    {
        const char* selectSql = "SELECT setting_value FROM system_settings WHERE setting_key=?;";
        sqlite3_stmt* selectStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, selectSql, -1, &selectStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(selectStmt, 1, Setting.SettingKey.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(selectStmt) == SQLITE_ROW) {
                oldValue = SafeCStr((const char*)sqlite3_column_text(selectStmt, 0), "");
            }
        }
        sqlite3_finalize(selectStmt);
    }

    const char* sql =
        "INSERT INTO system_settings (setting_key, setting_value, value_type, category, is_sensitive, description, updated_by, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now')) "
        "ON CONFLICT(setting_key) DO UPDATE SET "
        "setting_value=excluded.setting_value, value_type=excluded.value_type, category=excluded.category, "
        "is_sensitive=excluded.is_sensitive, description=excluded.description, updated_by=excluded.updated_by, updated_at=excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, Setting.SettingKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, Setting.SettingValue.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, Setting.ValueType.empty() ? "string" : Setting.ValueType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, Setting.Category.empty() ? "general" : Setting.Category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, Setting.bSensitive ? 1 : 0);
    sqlite3_bind_text(stmt, 6, Setting.Description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, UpdatedBy.empty() ? "system" : UpdatedBy.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return false;

    if (OutEvent) {
        OutEvent->SettingKey = Setting.SettingKey;
        OutEvent->OldValue = oldValue;
        OutEvent->NewValue = Setting.SettingValue;
        OutEvent->ValueType = Setting.ValueType.empty() ? "string" : Setting.ValueType;
        OutEvent->Category = Setting.Category.empty() ? "general" : Setting.Category;
        OutEvent->bSensitive = Setting.bSensitive;
        OutEvent->UpdatedBy = UpdatedBy.empty() ? "system" : UpdatedBy;
        OutEvent->UpdatedAtUnix = static_cast<int64_t>(std::time(nullptr));
    }
    return true;
}

bool LedgerManager::DeleteSystemSetting(const std::string& SettingKey,
                                        std::string& OutOldValue)
{
    lock_guard<mutex> lock(m_Mutex);
    OutOldValue.clear();
    if (!m_Db || SettingKey.empty()) return false;

    {
        const char* querySql = "SELECT setting_value FROM system_settings WHERE setting_key=? LIMIT 1;";
        sqlite3_stmt* queryStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, querySql, -1, &queryStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(queryStmt, 1, SettingKey.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(queryStmt) == SQLITE_ROW) {
                const unsigned char* text = sqlite3_column_text(queryStmt, 0);
                OutOldValue = text ? reinterpret_cast<const char*>(text) : "";
            }
        }
        sqlite3_finalize(queryStmt);
    }

    const char* sql = "DELETE FROM system_settings WHERE setting_key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, SettingKey.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// ============================================================================
// 用户管理
// ============================================================================

bool LedgerManager::AuthenticateUser(const string& Username,
                                      const string& PasswordHash,
                                      string& OutPermissions)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    bool found = false;
    string sql = "SELECT password_hash, permissions FROM users WHERE username=? AND is_active=1;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_Db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* storedHash = (const char*)sqlite3_column_text(stmt, 0);
        const char* storedPerms = (const char*)sqlite3_column_text(stmt, 1);

        if (storedHash && PasswordHash == string(storedHash)) {
            OutPermissions = NormalizePermissionsJson(storedPerms ? storedPerms : "[]");
            found = true;
        }
        // 空密码处理：首次登录初始化，并立即写回本次密码哈希，避免长期空密码。
        if (!found && storedHash && string(storedHash).empty() && !PasswordHash.empty()) {
            const char* updateSql = "UPDATE users SET password_hash=? WHERE username=? AND is_active=1;";
            sqlite3_stmt* updateStmt = nullptr;
            if (sqlite3_prepare_v2(m_Db, updateSql, -1, &updateStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(updateStmt, 1, PasswordHash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(updateStmt, 2, Username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(updateStmt) == SQLITE_DONE && sqlite3_changes(m_Db) > 0) {
                    found = true;
                    OutPermissions = NormalizePermissionsJson(storedPerms ? storedPerms : "[]");
                    printf("[Ledger] Initialized empty password_hash for user '%s'\n", Username.c_str());
                }
                sqlite3_finalize(updateStmt);
            }
        }
    }

    sqlite3_finalize(stmt);

    return found;
}

bool LedgerManager::GetUserPasswordHash(const string& Username, string& OutPasswordHash)
{
    lock_guard<mutex> lock(m_Mutex);
    OutPasswordHash.clear();
    if (!m_Db) return false;

    const char* sql = "SELECT password_hash FROM users WHERE username=? AND is_active=1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OutPasswordHash = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "");
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

bool LedgerManager::GetUserByUsername(const string& Username, string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    bool found = false;
    string sql = "SELECT id, username, permissions, pid, is_active FROM users WHERE username=?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_Db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ostringstream oss;
        oss << "{"
            << "\"id\":" << sqlite3_column_int(stmt, 0) << ","
            << "\"username\":\"" << EscapeJsonString(
                   (const char*)sqlite3_column_text(stmt, 1)) << "\","
            << "\"permissions\":" << (
                   SafeCStr((const char*)sqlite3_column_text(stmt, 2), "[]")) << ","
            << "\"pid\":\"" << EscapeJsonString(
                   SafeCStr((const char*)sqlite3_column_text(stmt, 3), "")) << "\","
            << "\"isActive\":" << sqlite3_column_int(stmt, 4)
            << "}";
        OutJson = oss.str();
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool LedgerManager::GetLedgerPidBindings(const string& Username, string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    if (!m_Db) return false;
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId <= 0) return false;

    const char* sql =
        "SELECT pb.id,pb.pid,pb.ledger_id,l.name,f.name,pb.created_at,pb.updated_at,"
        "CASE WHEN fm.id IS NULL THEN 0 ELSE 1 END "
        "FROM ledger_pid_bindings pb "
        "JOIN ledgers l ON l.id=pb.ledger_id JOIN families f ON f.id=l.family_id "
        "LEFT JOIN family_members fm ON fm.family_id=l.family_id AND fm.user_id=pb.created_by "
        "WHERE pb.created_by=? ORDER BY pb.id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, userId);
    ostringstream out;
    out << "[";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) out << ",";
        first = false;
        out << "{\"id\":" << sqlite3_column_int(stmt, 0)
            << ",\"pid\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 1), "")) << "\""
            << ",\"ledgerId\":" << sqlite3_column_int(stmt, 2)
            << ",\"ledgerName\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 3), "")) << "\""
            << ",\"familyName\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 4), "")) << "\""
            << ",\"createdAt\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 5), "")) << "\""
            << ",\"updatedAt\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 6), "")) << "\""
            << ",\"accessible\":" << (sqlite3_column_int(stmt, 7) != 0 ? "true" : "false") << "}";
    }
    sqlite3_finalize(stmt);
    out << "]";
    OutJson = out.str();
    return true;
}

bool LedgerManager::CreateLedgerPidBinding(const string& Username, const string& Pid,
                                           int LedgerId, string& OutJson, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson.clear(); OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (Pid.empty() || Pid.size() > 80) { OutError = "PID must contain 1 to 80 characters"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId <= 0) { OutError = "User not found"; return false; }
    if (LedgerId <= 0 || !CanAccessLedgerNoLock(m_Db, LedgerId, userId)) { OutError = "Current user cannot access ledger"; return false; }
    const char* sql = "INSERT INTO ledger_pid_bindings(pid,ledger_id,created_by) VALUES(?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to prepare PID creation"; return false; }
    sqlite3_bind_text(stmt, 1, Pid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, LedgerId);
    sqlite3_bind_int(stmt, 3, userId);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { OutError = rc == SQLITE_CONSTRAINT ? "PID already exists" : "Failed to create PID"; return false; }
    const int bindingId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
    ostringstream out; out << "{\"id\":" << bindingId << ",\"pid\":\"" << EscapeJsonString(Pid)
                           << "\",\"ledgerId\":" << LedgerId << "}";
    OutJson = out.str();
    return true;
}

bool LedgerManager::UpdateLedgerPidBindingLedger(const string& Username, int BindingId,
                                                  int LedgerId, string& OutJson, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson.clear(); OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId <= 0 || !CanAccessLedgerNoLock(m_Db, LedgerId, userId)) { OutError = "Current user cannot access ledger"; return false; }
    const char* sql = "UPDATE ledger_pid_bindings SET ledger_id=?,updated_at=datetime('now','localtime') WHERE id=? AND created_by=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to prepare PID update"; return false; }
    sqlite3_bind_int(stmt, 1, LedgerId); sqlite3_bind_int(stmt, 2, BindingId); sqlite3_bind_int(stmt, 3, userId);
    const int rc = sqlite3_step(stmt); const int changed = sqlite3_changes(m_Db); sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changed != 1) { OutError = "PID binding not found"; return false; }
    ostringstream out; out << "{\"id\":" << BindingId << ",\"ledgerId\":" << LedgerId << "}"; OutJson = out.str();
    return true;
}

bool LedgerManager::DeleteLedgerPidBinding(const string& Username, int BindingId, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId <= 0) { OutError = "User not found"; return false; }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, "DELETE FROM ledger_pid_bindings WHERE id=? AND created_by=?;", -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to prepare PID deletion"; return false; }
    sqlite3_bind_int(stmt, 1, BindingId); sqlite3_bind_int(stmt, 2, userId);
    const int rc = sqlite3_step(stmt); const int changed = sqlite3_changes(m_Db); sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changed != 1) { OutError = "PID binding not found"; return false; }
    return true;
}

bool LedgerManager::ResolveLedgerPidRoute(const string& Pid, FLedgerPidRoute& OutRoute, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutRoute = FLedgerPidRoute{}; OutError.clear();
    if (!m_Db || Pid.empty()) { OutError = "PID not found"; return false; }
    const char* sql =
        "SELECT pb.id,pb.ledger_id,u.id,u.username FROM ledger_pid_bindings pb "
        "JOIN users u ON u.id=pb.created_by AND u.is_active=1 "
        "JOIN ledgers l ON l.id=pb.ledger_id "
        "JOIN family_members fm ON fm.family_id=l.family_id AND fm.user_id=u.id "
        "WHERE pb.pid=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to resolve PID"; return false; }
    sqlite3_bind_text(stmt, 1, Pid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OutRoute.BindingId = sqlite3_column_int(stmt, 0); OutRoute.LedgerId = sqlite3_column_int(stmt, 1);
        OutRoute.CreatorUserId = sqlite3_column_int(stmt, 2); OutRoute.CreatorUsername = SafeCStr((const char*)sqlite3_column_text(stmt, 3), "");
    }
    sqlite3_finalize(stmt);
    if (OutRoute.BindingId <= 0 || OutRoute.LedgerId <= 0 || OutRoute.CreatorUserId <= 0 || OutRoute.CreatorUsername.empty()) { OutError = "PID not found or inactive"; return false; }
    return true;
}

bool LedgerManager::GetAllUsers(string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson.clear();
    if (!m_Db) return false;

    ostringstream arr;
    arr << "[";
    bool first = true;

    // 用户列表必须读取新版家庭/账本上下文字段；旧 current_group_id 已从 V2 架构移除。
    const int rowCount = QueryRows(
        m_Db,
        "SELECT id, username, permissions, pid, current_family_id, current_ledger_id, is_active, "
        "created_at, last_login, CASE WHEN password_hash<>'' THEN 1 ELSE 0 END "
        "FROM users ORDER BY id",
        [&](sqlite3_stmt* stmt) {
            if (!first) arr << ",";
            first = false;
            arr << "{"
                << "\"id\":" << sqlite3_column_int(stmt, 0) << ","
                << "\"username\":\"" << EscapeJsonString(
                       (const char*)sqlite3_column_text(stmt, 1)) << "\","
                << "\"permissions\":" << (
                       SafeCStr((const char*)sqlite3_column_text(stmt, 2), "[]")) << ","
                << "\"pid\":\"" << EscapeJsonString(
                       SafeCStr((const char*)sqlite3_column_text(stmt, 3), "")) << "\","
                << "\"currentFamilyId\":" << sqlite3_column_int(stmt, 4) << ","
                << "\"currentLedgerId\":" << sqlite3_column_int(stmt, 5) << ","
                // 迁移期兼容旧 Web 字段，其语义映射到当前账本而非家庭。
                << "\"currentGroupId\":" << sqlite3_column_int(stmt, 5) << ","
                << "\"isActive\":" << sqlite3_column_int(stmt, 6) << ","
                << "\"createdAt\":\"" << EscapeJsonString(
                       SafeCStr((const char*)sqlite3_column_text(stmt, 7), "")) << "\","
                << "\"lastLogin\":\"" << EscapeJsonString(
                       SafeCStr((const char*)sqlite3_column_text(stmt, 8), "")) << "\","
                << "\"hasPassword\":" << (sqlite3_column_int(stmt, 9) != 0 ? "true" : "false")
                << "}";
        });
    if (rowCount < 0) {
        return false;
    }

    arr << "]";
    OutJson = arr.str();
    return true;
}

bool LedgerManager::CreateUser(const string& Username,
                                const string& PasswordHash,
                                const string& Permissions)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql =
        "INSERT INTO users (username, password_hash, permissions, is_active) "
        "VALUES (?, ?, ?, 1);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    const string normalizedPermissions = NormalizePermissionsJson(Permissions);

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, PasswordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, normalizedPermissions.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool LedgerManager::UpdateUserPassword(const string& Username,
                                        const string& NewPasswordHash)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql = "UPDATE users SET password_hash=? WHERE username=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, NewPasswordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, Username.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

bool LedgerManager::UpdateUserPasswordAndInvalidateSessions(
    const string& Username,
    const string& NewPasswordHash,
    int& OutDeletedSessionCount)
{
    lock_guard<mutex> lock(m_Mutex);
    OutDeletedSessionCount = 0;
    if (!m_Db || Username.empty()) return false;

    // 密码更新与会话注销必须同生共死，避免新密码已生效但旧 Token 仍然可用。
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] UpdateUserPasswordAndInvalidateSessions begin: %s\n",
                sqlite3_errmsg(m_Db));
        return false;
    }

    auto Rollback = [this]() {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    sqlite3_stmt* updateStmt = nullptr;
    const char* updateSql =
        "UPDATE users SET password_hash=? WHERE username=? AND is_active=1;";
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &updateStmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] UpdateUserPasswordAndInvalidateSessions update prepare: %s\n",
                sqlite3_errmsg(m_Db));
        Rollback();
        return false;
    }

    sqlite3_bind_text(updateStmt, 1, NewPasswordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateStmt, 2, Username.c_str(), -1, SQLITE_TRANSIENT);
    const int updateRc = sqlite3_step(updateStmt);
    sqlite3_finalize(updateStmt);
    if (updateRc != SQLITE_DONE || sqlite3_changes(m_Db) <= 0) {
        Rollback();
        return false;
    }

    sqlite3_stmt* deleteStmt = nullptr;
    const char* deleteSql = "DELETE FROM auth_sessions WHERE username=?;";
    if (sqlite3_prepare_v2(m_Db, deleteSql, -1, &deleteStmt, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] UpdateUserPasswordAndInvalidateSessions delete prepare: %s\n",
                sqlite3_errmsg(m_Db));
        Rollback();
        return false;
    }

    sqlite3_bind_text(deleteStmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);
    const int deleteRc = sqlite3_step(deleteStmt);
    if (deleteRc == SQLITE_DONE) {
        OutDeletedSessionCount = sqlite3_changes(m_Db);
    }
    sqlite3_finalize(deleteStmt);
    if (deleteRc != SQLITE_DONE) {
        OutDeletedSessionCount = 0;
        Rollback();
        return false;
    }

    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        fprintf(stderr, "[Ledger] UpdateUserPasswordAndInvalidateSessions commit: %s\n",
                sqlite3_errmsg(m_Db));
        OutDeletedSessionCount = 0;
        Rollback();
        return false;
    }
    return true;
}

bool LedgerManager::AdminUpdateUserPassword(
    const string& ActorUsername,
    const string& TargetUsername,
    const string& NewPasswordHash,
    FUserAccountMutationResult& OutResult)
{
    lock_guard<mutex> lock(m_Mutex);
    OutResult = FUserAccountMutationResult{};
    if (!m_Db) {
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Database unavailable";
        return false;
    }
    if (ActorUsername.empty() || TargetUsername.empty()) {
        OutResult.ErrorCode = "INVALID_USERNAME";
        OutResult.Message = "Actor and target username are required";
        return false;
    }
    if (ActorUsername == TargetUsername) {
        OutResult.ErrorCode = "SELF_OPERATION_FORBIDDEN";
        OutResult.Message = "Administrator cannot manage own password here";
        return false;
    }

    // 管理操作必须在数据库层再次校验管理员身份，不能只依赖 Web 层缓存。
    sqlite3_stmt* actorStmt = nullptr;
    const char* actorSql =
        "SELECT 1 FROM users WHERE username=? AND is_active=1 "
        "AND instr(permissions, '\"admin\"')>0 LIMIT 1;";
    bool actorIsAdmin = false;
    if (sqlite3_prepare_v2(m_Db, actorSql, -1, &actorStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(actorStmt, 1, ActorUsername.c_str(), -1, SQLITE_TRANSIENT);
        actorIsAdmin = sqlite3_step(actorStmt) == SQLITE_ROW;
    }
    sqlite3_finalize(actorStmt);
    if (!actorIsAdmin) {
        OutResult.ErrorCode = "ADMIN_REQUIRED";
        OutResult.Message = "Active administrator permission is required";
        return false;
    }

    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to begin password transaction";
        return false;
    }
    auto Rollback = [this]() { sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr); };

    sqlite3_stmt* updateStmt = nullptr;
    const char* updateSql = "UPDATE users SET password_hash=? WHERE username=? AND is_active=1;";
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &updateStmt, nullptr) != SQLITE_OK) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to prepare password update";
        return false;
    }
    sqlite3_bind_text(updateStmt, 1, NewPasswordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateStmt, 2, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
    const bool updated = sqlite3_step(updateStmt) == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
    sqlite3_finalize(updateStmt);
    if (!updated) {
        Rollback();
        OutResult.ErrorCode = "USER_NOT_FOUND";
        OutResult.Message = "Active target user was not found";
        return false;
    }

    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, "DELETE FROM auth_sessions WHERE username=?;", -1, &deleteStmt, nullptr) != SQLITE_OK) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to prepare session invalidation";
        return false;
    }
    sqlite3_bind_text(deleteStmt, 1, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
    const bool deleted = sqlite3_step(deleteStmt) == SQLITE_DONE;
    OutResult.DeletedSessionCount = deleted ? sqlite3_changes(m_Db) : 0;
    sqlite3_finalize(deleteStmt);
    if (!deleted || sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        OutResult.DeletedSessionCount = 0;
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to commit password update";
        return false;
    }

    OutResult.bOk = true;
    OutResult.bIsActive = true;
    return true;
}

bool LedgerManager::SetUserActiveState(
    const string& ActorUsername,
    const string& TargetUsername,
    bool bActive,
    FUserAccountMutationResult& OutResult)
{
    lock_guard<mutex> lock(m_Mutex);
    OutResult = FUserAccountMutationResult{};
    if (!m_Db) {
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Database unavailable";
        return false;
    }
    if (ActorUsername.empty() || TargetUsername.empty()) {
        OutResult.ErrorCode = "INVALID_USERNAME";
        OutResult.Message = "Actor and target username are required";
        return false;
    }
    if (ActorUsername == TargetUsername) {
        OutResult.ErrorCode = "SELF_OPERATION_FORBIDDEN";
        OutResult.Message = "Administrator cannot change own account state";
        return false;
    }

    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to begin account state transaction";
        return false;
    }
    auto Rollback = [this]() { sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr); };

    // 在同一写事务内校验操作者和最后一个管理员，避免并发管理操作穿透保护。
    sqlite3_stmt* actorStmt = nullptr;
    const char* actorSql =
        "SELECT 1 FROM users WHERE username=? AND is_active=1 "
        "AND instr(permissions, '\"admin\"')>0 LIMIT 1;";
    bool actorIsAdmin = false;
    if (sqlite3_prepare_v2(m_Db, actorSql, -1, &actorStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(actorStmt, 1, ActorUsername.c_str(), -1, SQLITE_TRANSIENT);
        actorIsAdmin = sqlite3_step(actorStmt) == SQLITE_ROW;
    }
    sqlite3_finalize(actorStmt);
    if (!actorIsAdmin) {
        Rollback();
        OutResult.ErrorCode = "ADMIN_REQUIRED";
        OutResult.Message = "Active administrator permission is required";
        return false;
    }

    sqlite3_stmt* targetStmt = nullptr;
    bool targetExists = false;
    bool targetIsAdmin = false;
    if (sqlite3_prepare_v2(m_Db,
            "SELECT permissions FROM users WHERE username=? LIMIT 1;",
            -1, &targetStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(targetStmt, 1, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(targetStmt) == SQLITE_ROW) {
            targetExists = true;
            const string permissions = SafeCStr((const char*)sqlite3_column_text(targetStmt, 0), "[]");
            targetIsAdmin = permissions.find("\"admin\"") != string::npos;
        }
    }
    sqlite3_finalize(targetStmt);
    if (!targetExists) {
        Rollback();
        OutResult.ErrorCode = "USER_NOT_FOUND";
        OutResult.Message = "Target user was not found";
        return false;
    }

    if (!bActive && targetIsAdmin) {
        sqlite3_stmt* countStmt = nullptr;
        int activeAdminCount = 0;
        if (sqlite3_prepare_v2(m_Db,
                "SELECT COUNT(*) FROM users WHERE is_active=1 AND instr(permissions, '\"admin\"')>0;",
                -1, &countStmt, nullptr) == SQLITE_OK && sqlite3_step(countStmt) == SQLITE_ROW) {
            activeAdminCount = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
        if (activeAdminCount <= 1) {
            Rollback();
            OutResult.ErrorCode = "LAST_ACTIVE_ADMIN";
            OutResult.Message = "At least one active administrator must remain";
            return false;
        }
    }

    sqlite3_stmt* updateStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, "UPDATE users SET is_active=? WHERE username=?;", -1, &updateStmt, nullptr) != SQLITE_OK) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to prepare account state update";
        return false;
    }
    sqlite3_bind_int(updateStmt, 1, bActive ? 1 : 0);
    sqlite3_bind_text(updateStmt, 2, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
    const bool updated = sqlite3_step(updateStmt) == SQLITE_DONE;
    sqlite3_finalize(updateStmt);
    if (!updated) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to update account state";
        return false;
    }

    if (!bActive) {
        sqlite3_stmt* deleteStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM auth_sessions WHERE username=?;", -1, &deleteStmt, nullptr) != SQLITE_OK) {
            Rollback();
            OutResult.ErrorCode = "DATABASE_ERROR";
            OutResult.Message = "Failed to prepare session invalidation";
            return false;
        }
        sqlite3_bind_text(deleteStmt, 1, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
        const bool deleted = sqlite3_step(deleteStmt) == SQLITE_DONE;
        OutResult.DeletedSessionCount = deleted ? sqlite3_changes(m_Db) : 0;
        sqlite3_finalize(deleteStmt);
        if (!deleted) {
            Rollback();
            OutResult.DeletedSessionCount = 0;
            OutResult.ErrorCode = "DATABASE_ERROR";
            OutResult.Message = "Failed to invalidate sessions";
            return false;
        }
    }

    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        Rollback();
        OutResult.DeletedSessionCount = 0;
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to commit account state update";
        return false;
    }
    OutResult.bOk = true;
    OutResult.bIsActive = bActive;
    return true;
}

bool LedgerManager::DeleteUserAccount(
    const string& ActorUsername,
    const string& TargetUsername,
    bool bHardDelete,
    FUserAccountMutationResult& OutResult)
{
    if (!bHardDelete) {
        return SetUserActiveState(ActorUsername, TargetUsername, false, OutResult);
    }

    lock_guard<mutex> lock(m_Mutex);
    OutResult = FUserAccountMutationResult{};
    if (!m_Db) {
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Database unavailable";
        return false;
    }
    if (ActorUsername.empty() || TargetUsername.empty()) {
        OutResult.ErrorCode = "INVALID_USERNAME";
        OutResult.Message = "Actor and target username are required";
        return false;
    }
    if (ActorUsername == TargetUsername) {
        OutResult.ErrorCode = "SELF_OPERATION_FORBIDDEN";
        OutResult.Message = "Administrator cannot delete own account";
        return false;
    }

    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to begin delete transaction";
        return false;
    }
    auto Rollback = [this]() { sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr); };

    sqlite3_stmt* actorStmt = nullptr;
    bool actorIsAdmin = false;
    if (sqlite3_prepare_v2(m_Db,
            "SELECT 1 FROM users WHERE username=? AND is_active=1 AND instr(permissions, '\"admin\"')>0 LIMIT 1;",
            -1, &actorStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(actorStmt, 1, ActorUsername.c_str(), -1, SQLITE_TRANSIENT);
        actorIsAdmin = sqlite3_step(actorStmt) == SQLITE_ROW;
    }
    sqlite3_finalize(actorStmt);
    if (!actorIsAdmin) {
        Rollback();
        OutResult.ErrorCode = "ADMIN_REQUIRED";
        OutResult.Message = "Active administrator permission is required";
        return false;
    }

    sqlite3_stmt* userStmt = nullptr;
    int userId = -1;
    bool targetIsActiveAdmin = false;
    if (sqlite3_prepare_v2(m_Db,
            "SELECT id, is_active, permissions FROM users WHERE username=? LIMIT 1;",
            -1, &userStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(userStmt, 1, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(userStmt) == SQLITE_ROW) {
            userId = sqlite3_column_int(userStmt, 0);
            const bool active = sqlite3_column_int(userStmt, 1) != 0;
            const string permissions = SafeCStr((const char*)sqlite3_column_text(userStmt, 2), "[]");
            targetIsActiveAdmin = active && permissions.find("\"admin\"") != string::npos;
        }
    }
    sqlite3_finalize(userStmt);
    if (userId < 0) {
        Rollback();
        OutResult.ErrorCode = "USER_NOT_FOUND";
        OutResult.Message = "Target user was not found";
        return false;
    }

    if (targetIsActiveAdmin) {
        sqlite3_stmt* countStmt = nullptr;
        int activeAdminCount = 0;
        if (sqlite3_prepare_v2(m_Db,
                "SELECT COUNT(*) FROM users WHERE is_active=1 AND instr(permissions, '\"admin\"')>0;",
                -1, &countStmt, nullptr) == SQLITE_OK && sqlite3_step(countStmt) == SQLITE_ROW) {
            activeAdminCount = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
        if (activeAdminCount <= 1) {
            Rollback();
            OutResult.ErrorCode = "LAST_ACTIVE_ADMIN";
            OutResult.Message = "At least one active administrator must remain";
            return false;
        }
    }

    // 任一业务归属存在都拒绝物理删除；认证会话不属于业务历史，可在删除时级联清理。
    const char* referenceSql =
        "SELECT "
        "(SELECT COUNT(*) FROM families WHERE created_by=?)+"
        "(SELECT COUNT(*) FROM family_members WHERE user_id=?)+"
        "(SELECT COUNT(*) FROM ledgers WHERE created_by=?)+"
        "(SELECT COUNT(*) FROM transactions WHERE created_by=?)+"
        "(SELECT COUNT(*) FROM invite_registrations WHERE created_by_user_id=?)+"
        "(SELECT COUNT(*) FROM pending_actions WHERE created_by_user_id=? OR target_user_id=?)+"
        "(SELECT COUNT(*) FROM bill_import_tasks WHERE username=?);";
    sqlite3_stmt* refStmt = nullptr;
    int referenceCount = -1;
    if (sqlite3_prepare_v2(m_Db, referenceSql, -1, &refStmt, nullptr) == SQLITE_OK) {
        for (int index = 1; index <= 7; ++index) {
            sqlite3_bind_int(refStmt, index, userId);
        }
        sqlite3_bind_text(refStmt, 8, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(refStmt) == SQLITE_ROW) {
            referenceCount = sqlite3_column_int(refStmt, 0);
        }
    }
    sqlite3_finalize(refStmt);
    if (referenceCount < 0) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to inspect user references";
        return false;
    }
    if (referenceCount > 0) {
        Rollback();
        OutResult.ErrorCode = "USER_HAS_REFERENCES";
        OutResult.Message = "User has business history and can only be soft deleted";
        OutResult.bCanHardDelete = false;
        return false;
    }

    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, "DELETE FROM users WHERE username=?;", -1, &deleteStmt, nullptr) != SQLITE_OK) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to prepare user deletion";
        return false;
    }
    sqlite3_bind_text(deleteStmt, 1, TargetUsername.c_str(), -1, SQLITE_TRANSIENT);
    const bool deleted = sqlite3_step(deleteStmt) == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
    sqlite3_finalize(deleteStmt);
    if (!deleted || sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        Rollback();
        OutResult.ErrorCode = "DATABASE_ERROR";
        OutResult.Message = "Failed to commit user deletion";
        return false;
    }

    OutResult.bOk = true;
    OutResult.bIsActive = false;
    OutResult.bCanHardDelete = true;
    return true;
}

bool LedgerManager::UpdateUserPermissions(const string& Username,
                                           const string& Permissions)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql = "UPDATE users SET permissions=? WHERE username=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    const string normalizedPermissions = NormalizePermissionsJson(Permissions);
    sqlite3_bind_text(stmt, 1, normalizedPermissions.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, Username.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}


bool LedgerManager::DeleteUser(const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql = "DELETE FROM users WHERE username=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

// ============================================================================
// 邀请注册
// ============================================================================

bool LedgerManager::SaveInviteRegistration(const string& Code,
                                           bool bAutoJoinFamily,
                                           int FamilyId,
                                           int DefaultLedgerId,
                                           const string& FamilyRole,
                                           const string& CreatedBy,
                                           bool bAllowNonAdminIndependentInvite,
                                           int64_t ExpiresAt,
                                           string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (Code.empty()) { OutError = "Missing invite code"; return false; }
    if (ExpiresAt <= static_cast<int64_t>(time(nullptr))) { OutError = "Invite expiration must be in the future"; return false; }
    if (!IsValidFamilyRole(FamilyRole)) { OutError = "Family role must be owner or member"; return false; }

    const int creatorId = GetUserIdNoLock(m_Db, CreatedBy);
    if (creatorId < 0) { OutError = "Creator user not found"; return false; }

    string permissions;
    const char* permissionSql = "SELECT permissions FROM users WHERE id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, permissionSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query creator permissions";
        return false;
    }
    sqlite3_bind_int(stmt, 1, creatorId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        permissions = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "[]");
    }
    sqlite3_finalize(stmt);
    const bool bSystemAdmin = PermissionArrayContains(permissions, "admin");

    if (!bAutoJoinFamily) {
        FamilyId = 0;
        DefaultLedgerId = 0;
        if (!bSystemAdmin && !bAllowNonAdminIndependentInvite) {
            OutError = "只有系统管理员才能创建独立用户";
            return false;
        }
        // 非系统管理员仍必须具备至少一个家庭 owner 身份，禁止普通 member 借独立邀请参数扩权。
        if (!bSystemAdmin) {
            const char* ownerSql =
                "SELECT 1 FROM family_members WHERE user_id=? AND role='owner' LIMIT 1;";
            stmt = nullptr;
            if (sqlite3_prepare_v2(m_Db, ownerSql, -1, &stmt, nullptr) != SQLITE_OK) {
                OutError = "Failed to verify family owner role";
                return false;
            }
            sqlite3_bind_int(stmt, 1, creatorId);
            const bool bOwnsFamily = sqlite3_step(stmt) == SQLITE_ROW;
            sqlite3_finalize(stmt);
            if (!bOwnsFamily) {
                OutError = "Only family owner or system admin can create registration invite";
                return false;
            }
        }
    } else {
        // 迁移期允许调用方只提供默认账本，家庭 ID 必须由服务端按账本归属推导，不能信任客户端拼接关系。
        if (FamilyId <= 0 && DefaultLedgerId > 0) {
            FamilyId = GetLedgerFamilyIdNoLock(m_Db, DefaultLedgerId);
        }
        if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }
        if (!bSystemAdmin && GetFamilyRoleNoLock(m_Db, FamilyId, creatorId) != "owner") {
            OutError = "Only family owner or system admin can create registration invite";
            return false;
        }
        if (DefaultLedgerId > 0 && GetLedgerFamilyIdNoLock(m_Db, DefaultLedgerId) != FamilyId) {
            OutError = "Default ledger does not belong to family";
            return false;
        }
    }

    const char* sql =
        "INSERT INTO invite_registrations "
        "(code, auto_join_family, family_id, default_ledger_id, family_role, created_by_user_id, created_at, expires_at) "
        "VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'), ?) "
        "ON CONFLICT(code) DO UPDATE SET "
        "auto_join_family=excluded.auto_join_family, family_id=excluded.family_id, "
        "default_ledger_id=excluded.default_ledger_id, family_role=excluded.family_role, "
        "created_by_user_id=excluded.created_by_user_id, created_at=excluded.created_at, "
        "expires_at=excluded.expires_at;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare invite save";
        return false;
    }
    sqlite3_bind_text(stmt, 1, Code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, bAutoJoinFamily ? 1 : 0);
    if (FamilyId > 0) sqlite3_bind_int(stmt, 3, FamilyId); else sqlite3_bind_null(stmt, 3);
    if (DefaultLedgerId > 0) sqlite3_bind_int(stmt, 4, DefaultLedgerId); else sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, FamilyRole.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, creatorId);
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(ExpiresAt));
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to save invite registration";
        return false;
    }
    return true;
}

static void FillInviteRegistrationFromStmt(sqlite3_stmt* stmt,
                                           FLedgerInviteRegistration& OutInvite)
{
    OutInvite = FLedgerInviteRegistration{};
    OutInvite.Code = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "");
    OutInvite.bAutoJoinFamily = sqlite3_column_int(stmt, 1) != 0;
    OutInvite.FamilyId = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 2);
    OutInvite.DefaultLedgerId = sqlite3_column_type(stmt, 3) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 3);
    OutInvite.FamilyRole = SafeCStr((const char*)sqlite3_column_text(stmt, 4), "member");
    OutInvite.CreatedByUserId = sqlite3_column_int(stmt, 5);
    OutInvite.CreatedBy = SafeCStr((const char*)sqlite3_column_text(stmt, 6), "");
    OutInvite.CreatedAt = static_cast<int64_t>(sqlite3_column_int64(stmt, 7));
    OutInvite.ExpiresAt = static_cast<int64_t>(sqlite3_column_int64(stmt, 8));
}

bool LedgerManager::GetValidInviteRegistration(const string& Code,
                                               FLedgerInviteRegistration& OutInvite,
                                               string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (Code.empty()) { OutError = "Missing invite code"; return false; }

    const char* sql =
        "SELECT ir.code, ir.auto_join_family, ir.family_id, ir.default_ledger_id, "
        "ir.family_role, ir.created_by_user_id, u.username, ir.created_at, ir.expires_at "
        "FROM invite_registrations ir "
        "JOIN users u ON u.id=ir.created_by_user_id "
        "WHERE ir.code=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query invite";
        return false;
    }
    sqlite3_bind_text(stmt, 1, Code.c_str(), -1, SQLITE_TRANSIENT);
    const bool bFound = sqlite3_step(stmt) == SQLITE_ROW;
    if (bFound) FillInviteRegistrationFromStmt(stmt, OutInvite);
    sqlite3_finalize(stmt);

    if (!bFound || OutInvite.ExpiresAt <= static_cast<int64_t>(time(nullptr))) {
        OutError = "需要邀请注册，或者邀请链接已经过期";
        return false;
    }
    return true;
}

bool LedgerManager::HasOwnedFamily(const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) return false;

    const char* sql =
        "SELECT 1 FROM family_members "
        "WHERE user_id=? AND role='owner' LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, userId);
    const bool bOwnsFamily = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return bOwnsFamily;
}

bool LedgerManager::GetCurrentInviteRegistration(const string& CreatedBy,
                                                  bool bAutoJoinFamily,
                                                  int FamilyId,
                                                  int DefaultLedgerId,
                                                  const string& FamilyRole,
                                                  FLedgerInviteRegistration& OutInvite,
                                                  string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int creatorId = GetUserIdNoLock(m_Db, CreatedBy);
    if (creatorId < 0) { OutError = "Creator user not found"; return false; }
    if (!IsValidFamilyRole(FamilyRole)) { OutError = "Family role must be owner or member"; return false; }

    // 与保存路径保持相同的服务端作用域推导，避免客户端只传默认账本时无法复用已保存邀请。
    if (bAutoJoinFamily && FamilyId <= 0 && DefaultLedgerId > 0) {
        FamilyId = GetLedgerFamilyIdNoLock(m_Db, DefaultLedgerId);
    }
    if (bAutoJoinFamily && FamilyId <= 0) {
        OutError = "Missing familyId";
        return false;
    }
    if (bAutoJoinFamily && DefaultLedgerId > 0 &&
        GetLedgerFamilyIdNoLock(m_Db, DefaultLedgerId) != FamilyId) {
        OutError = "Default ledger does not belong to family";
        return false;
    }

    const char* sql =
        "SELECT ir.code, ir.auto_join_family, ir.family_id, ir.default_ledger_id, "
        "ir.family_role, ir.created_by_user_id, u.username, ir.created_at, ir.expires_at "
        "FROM invite_registrations ir JOIN users u ON u.id=ir.created_by_user_id "
        "WHERE ir.expires_at>? AND ir.created_by_user_id=? AND ir.auto_join_family=? "
        "AND COALESCE(ir.family_id,0)=? AND COALESCE(ir.default_ledger_id,0)=? AND ir.family_role=? "
        "ORDER BY ir.created_at DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query current invite";
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(time(nullptr)));
    sqlite3_bind_int(stmt, 2, creatorId);
    sqlite3_bind_int(stmt, 3, bAutoJoinFamily ? 1 : 0);
    sqlite3_bind_int(stmt, 4, bAutoJoinFamily ? FamilyId : 0);
    sqlite3_bind_int(stmt, 5, bAutoJoinFamily ? DefaultLedgerId : 0);
    sqlite3_bind_text(stmt, 6, FamilyRole.c_str(), -1, SQLITE_TRANSIENT);
    const bool bFound = sqlite3_step(stmt) == SQLITE_ROW;
    if (bFound) FillInviteRegistrationFromStmt(stmt, OutInvite);
    sqlite3_finalize(stmt);
    return bFound;
}

int LedgerManager::CleanupExpiredInviteRegistrations(int64_t Now)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return 0;
    const char* sql = "DELETE FROM invite_registrations WHERE expires_at<=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(Now));
    const int rc = sqlite3_step(stmt);
    const int changes = (rc == SQLITE_DONE) ? sqlite3_changes(m_Db) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

bool LedgerManager::ApplyInviteRegistrationToUser(const FLedgerInviteRegistration& Invite,
                                                  const string& Username,
                                                  string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }
    if (!Invite.bAutoJoinFamily) return true;
    if (Invite.FamilyId <= 0 || !IsValidFamilyRole(Invite.FamilyRole)) {
        OutError = "Invalid invite family scope";
        return false;
    }
    if (Invite.DefaultLedgerId > 0 && GetLedgerFamilyIdNoLock(m_Db, Invite.DefaultLedgerId) != Invite.FamilyId) {
        OutError = "Default ledger does not belong to family";
        return false;
    }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError) != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin invite apply transaction";
        sqlite3_free(txError);
        return false;
    }
    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = Error;
        return false;
    };

    const char* memberSql =
        "INSERT INTO family_members (family_id, user_id, role) VALUES (?, ?, ?) "
        "ON CONFLICT(family_id,user_id) DO NOTHING;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, memberSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare family join");
    sqlite3_bind_int(stmt, 1, Invite.FamilyId);
    sqlite3_bind_int(stmt, 2, userId);
    sqlite3_bind_text(stmt, 3, Invite.FamilyRole.c_str(), -1, SQLITE_TRANSIENT);
    const int memberRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (memberRc != SQLITE_DONE) return rollback("Failed to join family");

    const char* contextSql = "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare invite context update");
    sqlite3_bind_int(stmt, 1, Invite.FamilyId);
    if (Invite.DefaultLedgerId > 0) sqlite3_bind_int(stmt, 2, Invite.DefaultLedgerId); else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, userId);
    const int contextRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (contextRc != SQLITE_DONE) return rollback("Failed to set invite context");

    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit invite apply transaction";
        sqlite3_free(txError);
        return rollback(error);
    }
    return true;
}

bool LedgerManager::ApplyInviteCodeToExistingUser(const string& Code,
                                                  const string& Username,
                                                  FLedgerInviteRegistration& OutInvite,
                                                  string& OutError)
{
    if (!GetValidInviteRegistration(Code, OutInvite, OutError)) return false;
    return ApplyInviteRegistrationToUser(OutInvite, Username, OutError);
}

bool LedgerManager::RegisterUserWithInvite(const string& Code,
                                           const string& Username,
                                           const string& PasswordHash,
                                           const string& Permissions,
                                           FLedgerInviteRegistration& OutInvite,
                                           string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    OutInvite = FLedgerInviteRegistration{};
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (Code.empty() || Username.empty()) { OutError = "Missing code or username"; return false; }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError) != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin registration transaction";
        sqlite3_free(txError);
        return false;
    }
    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutInvite = FLedgerInviteRegistration{};
        OutError = Error;
        return false;
    };

    const char* inviteSql =
        "SELECT ir.code, ir.auto_join_family, ir.family_id, ir.default_ledger_id, "
        "ir.family_role, ir.created_by_user_id, u.username, ir.created_at, ir.expires_at "
        "FROM invite_registrations ir JOIN users u ON u.id=ir.created_by_user_id "
        "WHERE ir.code=? AND ir.expires_at>? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, inviteSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare invite query");
    sqlite3_bind_text(stmt, 1, Code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(time(nullptr)));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return rollback("需要邀请注册，或者邀请链接已经过期");
    }
    FillInviteRegistrationFromStmt(stmt, OutInvite);
    sqlite3_finalize(stmt);

    if (OutInvite.bAutoJoinFamily) {
        if (OutInvite.FamilyId <= 0 || !IsValidFamilyRole(OutInvite.FamilyRole)) return rollback("Invalid invite family scope");
        if (OutInvite.DefaultLedgerId > 0 && GetLedgerFamilyIdNoLock(m_Db, OutInvite.DefaultLedgerId) != OutInvite.FamilyId) {
            return rollback("Default ledger does not belong to family");
        }
    }

    const char* userSql =
        "INSERT INTO users (username, password_hash, permissions, is_active) VALUES (?, ?, ?, 1);";
    if (sqlite3_prepare_v2(m_Db, userSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare user creation");
    const string normalizedPermissions = NormalizePermissionsJson(Permissions);
    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, PasswordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, normalizedPermissions.c_str(), -1, SQLITE_TRANSIENT);
    const int userRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (userRc != SQLITE_DONE) return rollback("注册失败，用户名可能已经存在");
    const int userId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));

    if (OutInvite.bAutoJoinFamily) {
        const char* memberSql =
            "INSERT INTO family_members (family_id, user_id, role) VALUES (?, ?, ?);";
        if (sqlite3_prepare_v2(m_Db, memberSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare family join");
        sqlite3_bind_int(stmt, 1, OutInvite.FamilyId);
        sqlite3_bind_int(stmt, 2, userId);
        sqlite3_bind_text(stmt, 3, OutInvite.FamilyRole.c_str(), -1, SQLITE_TRANSIENT);
        const int memberRc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (memberRc != SQLITE_DONE) return rollback("Failed to join family");

        const char* contextSql = "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
        if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare registration context");
        sqlite3_bind_int(stmt, 1, OutInvite.FamilyId);
        if (OutInvite.DefaultLedgerId > 0) sqlite3_bind_int(stmt, 2, OutInvite.DefaultLedgerId); else sqlite3_bind_null(stmt, 2);
        sqlite3_bind_int(stmt, 3, userId);
        const int contextRc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (contextRc != SQLITE_DONE) return rollback("Failed to set registration context");
    }

    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit registration transaction";
        sqlite3_free(txError);
        return rollback(error);
    }
    return true;
}

// ============================================================================
// 待审批动作 / 成员邀请
// ============================================================================

static string NormalizeMemberRole(const string& Role)
{
    // V2 家庭模型只有 owner/member；邀请不得借此创建第二个 owner。
    return "member";
}

static string BuildMemberInvitePayloadJson(int FamilyId, const string& Role, const string& FamilyName)
{
    ostringstream oss;
    oss << "{\"familyId\":" << FamilyId
        << ",\"role\":\"" << JsonLite::EscapeString(NormalizeMemberRole(Role)) << "\""
        << ",\"familyName\":\"" << JsonLite::EscapeString(FamilyName) << "\"}";
    return oss.str();
}

static void FillMemberInviteFromStmt(sqlite3_stmt* stmt, FLedgerMemberInvite& OutInvite)
{
    OutInvite = FLedgerMemberInvite{};
    OutInvite.Base.Id = sqlite3_column_int(stmt, 0);
    OutInvite.Base.ActionType = SafeCStr((const char*)sqlite3_column_text(stmt, 1), "");
    OutInvite.Base.Status = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "");
    OutInvite.Base.CreatedByUserId = sqlite3_column_int(stmt, 3);
    OutInvite.Base.CreatedByUsername = SafeCStr((const char*)sqlite3_column_text(stmt, 4), "");
    OutInvite.Base.TargetUserId = sqlite3_column_int(stmt, 5);
    OutInvite.Base.TargetUsername = SafeCStr((const char*)sqlite3_column_text(stmt, 6), "");
    OutInvite.FamilyId = sqlite3_column_int(stmt, 7);
    OutInvite.Role = NormalizeMemberRole(SafeCStr((const char*)sqlite3_column_text(stmt, 8), "member"));
    OutInvite.Base.PayloadJson = SafeCStr((const char*)sqlite3_column_text(stmt, 9), "{}");
    OutInvite.Base.CreatedAt = static_cast<int64_t>(sqlite3_column_int64(stmt, 10));
    OutInvite.Base.ExpiresAt = static_cast<int64_t>(sqlite3_column_int64(stmt, 11));
    OutInvite.Base.RespondedAt = static_cast<int64_t>(sqlite3_column_int64(stmt, 12));
    OutInvite.FamilyName = SafeCStr((const char*)sqlite3_column_text(stmt, 13), "");
    if (OutInvite.FamilyId <= 0) {
        OutInvite.FamilyId = JsonLite::GetIntOrDefault(OutInvite.Base.PayloadJson, "familyId", 0);
    }
    if (OutInvite.FamilyName.empty()) {
        OutInvite.FamilyName = JsonLite::GetStringOrDefault(OutInvite.Base.PayloadJson, "familyName", "");
    }
    // WebSocket 协议仍以 groupId/groupName 命名；兼容字段映射为家庭作用域。
    OutInvite.GroupId = OutInvite.FamilyId;
    OutInvite.GroupName = OutInvite.FamilyName;
}

static string MemberInvitesToJson(const vector<FLedgerMemberInvite>& Invites)
{
    ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < Invites.size(); ++i) {
        const FLedgerMemberInvite& item = Invites[i];
        if (i > 0) oss << ",";
        oss << "{"
            << "\"id\":" << item.Base.Id << ","
            << "\"status\":\"" << JsonLite::EscapeString(item.Base.Status) << "\","
            << "\"familyId\":" << item.FamilyId << ","
            << "\"familyName\":\"" << JsonLite::EscapeString(item.FamilyName) << "\","
            << "\"groupId\":" << item.FamilyId << ","
            << "\"groupName\":\"" << JsonLite::EscapeString(item.FamilyName) << "\","
            << "\"role\":\"" << JsonLite::EscapeString(item.Role) << "\","
            << "\"invitedBy\":\"" << JsonLite::EscapeString(item.Base.CreatedByUsername) << "\","
            << "\"targetUsername\":\"" << JsonLite::EscapeString(item.Base.TargetUsername) << "\","
            << "\"createdAt\":" << item.Base.CreatedAt << ","
            << "\"expiresAt\":" << item.Base.ExpiresAt << ","
            << "\"respondedAt\":" << item.Base.RespondedAt
            << "}";
    }
    oss << "]";
    return oss.str();
}

bool LedgerManager::CreateMemberInvite(int GroupId,
                                       const string& InviteeUsername,
                                       const string& Role,
                                       const string& InvitedByUsername,
                                       int ExpiresInSec,
                                       FLedgerMemberInvite& OutInvite,
                                       string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutInvite = FLedgerMemberInvite{};
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (GroupId <= 0 || InviteeUsername.empty()) { OutError = "Missing groupId or username"; return false; }
    if (InviteeUsername == InvitedByUsername) { OutError = "Cannot invite yourself"; return false; }

    const int inviterId = GetUserIdNoLock(m_Db, InvitedByUsername);
    const int inviteeId = GetUserIdNoLock(m_Db, InviteeUsername);
    if (inviterId < 0) { OutError = "Inviter user not found"; return false; }
    if (inviteeId < 0) { OutError = "Target user not found"; return false; }

    // 兼容协议中的 groupId 实际代表账本 ID；领域授权必须提升到账本所属家庭。
    const int familyId = GetLedgerFamilyIdNoLock(m_Db, GroupId);
    if (familyId <= 0) { OutError = "Ledger not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, familyId, inviterId) != "owner") {
        OutError = "Only family owner can invite member";
        return false;
    }
    if (!GetFamilyRoleNoLock(m_Db, familyId, inviteeId).empty()) {
        OutError = "Target user is already a family member";
        return false;
    }

    string familyName;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, "SELECT name FROM families WHERE id=? LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query family";
        fprintf(stderr, "[Ledger] CreateMemberInvite family query prepare error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, familyId);
    if (sqlite3_step(stmt) == SQLITE_ROW) familyName = SafeCStr((const char*)sqlite3_column_text(stmt, 0), "");
    sqlite3_finalize(stmt);
    if (familyName.empty()) { OutError = "Family not found"; return false; }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t expiresAt = now + (ExpiresInSec > 0 ? ExpiresInSec : 86400);
    const string normalizedRole = NormalizeMemberRole(Role);
    const string payload = BuildMemberInvitePayloadJson(familyId, normalizedRole, familyName);
    const char* selectSql =
        "SELECT pa.id,pa.action_type,pa.status,pa.created_by_user_id,pa.created_by_username,"
        "pa.target_user_id,pa.target_username,pa.family_id,pa.role,pa.payload_json,pa.created_at,pa.expires_at,pa.responded_at,f.name "
        "FROM pending_actions pa LEFT JOIN families f ON f.id=pa.family_id ";

    const string duplicateSql = string(selectSql)
        + "WHERE pa.action_type='family_member_invite' AND pa.status='pending' AND pa.target_user_id=? "
          "AND pa.family_id=? AND (pa.expires_at=0 OR pa.expires_at>?) LIMIT 1;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, duplicateSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query pending invite";
        fprintf(stderr, "[Ledger] CreateMemberInvite duplicate query prepare error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, inviteeId);
    sqlite3_bind_int(stmt, 2, familyId);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(now));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        FillMemberInviteFromStmt(stmt, OutInvite);
        sqlite3_finalize(stmt);
        return true;
    }
    sqlite3_finalize(stmt);

    const char* insertSql =
        "INSERT INTO pending_actions (action_type,status,created_by_user_id,created_by_username,target_user_id,target_username,family_id,role,payload_json,created_at,expires_at,responded_at) "
        "VALUES ('family_member_invite','pending',?,?,?,?,?,?,?,?,?,0);";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to create invite";
        fprintf(stderr, "[Ledger] CreateMemberInvite insert prepare error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, inviterId);
    sqlite3_bind_text(stmt, 2, InvitedByUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, inviteeId);
    sqlite3_bind_text(stmt, 4, InviteeUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, familyId);
    sqlite3_bind_text(stmt, 6, normalizedRole.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(now));
    sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(expiresAt));
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to create invite";
        fprintf(stderr, "[Ledger] CreateMemberInvite insert error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }

    OutInvite.Base.Id = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
    OutInvite.Base.ActionType = "family_member_invite";
    OutInvite.Base.Status = "pending";
    OutInvite.Base.CreatedByUserId = inviterId;
    OutInvite.Base.CreatedByUsername = InvitedByUsername;
    OutInvite.Base.TargetUserId = inviteeId;
    OutInvite.Base.TargetUsername = InviteeUsername;
    OutInvite.Base.CreatedAt = now;
    OutInvite.Base.ExpiresAt = expiresAt;
    OutInvite.Base.PayloadJson = payload;
    OutInvite.FamilyId = familyId;
    OutInvite.FamilyName = familyName;
    OutInvite.Role = normalizedRole;
    OutInvite.GroupId = familyId;
    OutInvite.GroupName = familyName;
    return true;
}

bool LedgerManager::GetIncomingMemberInvites(const string& Username, string& OutJson, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }

    const char* sql =
        "SELECT pa.id,pa.action_type,pa.status,pa.created_by_user_id,pa.created_by_username,"
        "pa.target_user_id,pa.target_username,pa.family_id,pa.role,pa.payload_json,pa.created_at,pa.expires_at,pa.responded_at,f.name "
        "FROM pending_actions pa LEFT JOIN families f ON f.id=pa.family_id "
        "WHERE pa.action_type='family_member_invite' AND pa.status='pending' AND pa.target_user_id=? "
        "AND (pa.expires_at=0 OR pa.expires_at>?) ORDER BY pa.created_at DESC,pa.id DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query incoming invites";
        fprintf(stderr, "[Ledger] GetIncomingMemberInvites prepare error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, userId);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(time(nullptr)));
    vector<FLedgerMemberInvite> invites;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        FLedgerMemberInvite item;
        FillMemberInviteFromStmt(stmt, item);
        invites.push_back(item);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to query incoming invites";
        fprintf(stderr, "[Ledger] GetIncomingMemberInvites step error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    OutJson = MemberInvitesToJson(invites);
    return true;
}

bool LedgerManager::GetSentMemberInvites(int GroupId, const string& Username, string& OutJson, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (GroupId <= 0) { OutError = "Missing groupId"; return false; }
    const int requesterId = GetUserIdNoLock(m_Db, Username);
    if (requesterId < 0) { OutError = "User not found"; return false; }
    const int familyId = GetLedgerFamilyIdNoLock(m_Db, GroupId);
    if (familyId <= 0) { OutError = "Ledger not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, familyId, requesterId) != "owner") {
        OutError = "Only family owner can query sent invites";
        return false;
    }

    const char* sql =
        "SELECT pa.id,pa.action_type,pa.status,pa.created_by_user_id,pa.created_by_username,"
        "pa.target_user_id,pa.target_username,pa.family_id,pa.role,pa.payload_json,pa.created_at,pa.expires_at,pa.responded_at,f.name "
        "FROM pending_actions pa LEFT JOIN families f ON f.id=pa.family_id "
        "WHERE pa.action_type='family_member_invite' AND pa.status='pending' AND pa.family_id=? "
        "AND (pa.expires_at=0 OR pa.expires_at>?) ORDER BY pa.created_at DESC,pa.id DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query sent invites";
        fprintf(stderr, "[Ledger] GetSentMemberInvites prepare error: %s\n", sqlite3_errmsg(m_Db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(time(nullptr)));
    vector<FLedgerMemberInvite> invites;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        FLedgerMemberInvite item;
        FillMemberInviteFromStmt(stmt, item);
        invites.push_back(item);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { OutError = "Failed to query sent invites"; return false; }
    OutJson = MemberInvitesToJson(invites);
    return true;
}

bool LedgerManager::AcceptMemberInvite(int InviteId, const string& Username, int& OutGroupId, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutGroupId = 0;
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int targetUserId = GetUserIdNoLock(m_Db, Username);
    if (targetUserId < 0) { OutError = "User not found"; return false; }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    FLedgerMemberInvite invite;
    const char* inviteSql =
        "SELECT pa.id,pa.action_type,pa.status,pa.created_by_user_id,pa.created_by_username,"
        "pa.target_user_id,pa.target_username,pa.family_id,pa.role,pa.payload_json,pa.created_at,pa.expires_at,pa.responded_at,f.name "
        "FROM pending_actions pa LEFT JOIN families f ON f.id=pa.family_id "
        "WHERE pa.id=? AND pa.action_type='family_member_invite' LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, inviteSql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to query invite"; return false; }
    sqlite3_bind_int(stmt, 1, InviteId);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); OutError = "Invite not found"; return false; }
    FillMemberInviteFromStmt(stmt, invite);
    sqlite3_finalize(stmt);

    if (invite.Base.TargetUserId != targetUserId) { OutError = "Current user is not invite target"; return false; }
    if (invite.Base.Status != "pending") { OutError = "Invite has already been resolved"; return false; }
    if (invite.Base.ExpiresAt > 0 && invite.Base.ExpiresAt <= now) { OutError = "Invite has expired"; return false; }
    if (invite.FamilyId <= 0) { OutError = "Invalid invite family scope"; return false; }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError) != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin invite transaction";
        sqlite3_free(txError);
        return false;
    }
    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = Error;
        return false;
    };

    const char* memberSql =
        "INSERT INTO family_members (family_id,user_id,role) VALUES (?,?,'member') "
        "ON CONFLICT(family_id,user_id) DO NOTHING;";
    if (sqlite3_prepare_v2(m_Db, memberSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare family join");
    sqlite3_bind_int(stmt, 1, invite.FamilyId);
    sqlite3_bind_int(stmt, 2, targetUserId);
    const int memberRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (memberRc != SQLITE_DONE) return rollback("Failed to join family");

    // 当前账本选择家庭内最早创建的账本；没有账本时保持 NULL。
    int defaultLedgerId = 0;
    if (sqlite3_prepare_v2(m_Db, "SELECT id FROM ledgers WHERE family_id=? ORDER BY id LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to resolve default ledger");
    }
    sqlite3_bind_int(stmt, 1, invite.FamilyId);
    if (sqlite3_step(stmt) == SQLITE_ROW) defaultLedgerId = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char* contextSql = "UPDATE users SET current_family_id=?,current_ledger_id=? WHERE id=?;";
    if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare user context");
    sqlite3_bind_int(stmt, 1, invite.FamilyId);
    if (defaultLedgerId > 0) sqlite3_bind_int(stmt, 2, defaultLedgerId); else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, targetUserId);
    const int contextRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (contextRc != SQLITE_DONE) return rollback("Failed to update user context");

    const char* resolveSql =
        "UPDATE pending_actions SET status='accepted',responded_at=? "
        "WHERE id=? AND action_type='family_member_invite' AND status='pending';";
    if (sqlite3_prepare_v2(m_Db, resolveSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("Failed to prepare invite resolution");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(now));
    sqlite3_bind_int(stmt, 2, InviteId);
    const int resolveRc = sqlite3_step(stmt);
    const int resolvedRows = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (resolveRc != SQLITE_DONE || resolvedRows <= 0) return rollback("Invite has already been resolved");

    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit invite transaction";
        sqlite3_free(txError);
        return rollback(error);
    }
    OutGroupId = defaultLedgerId;
    return true;
}

bool LedgerManager::RejectMemberInvite(int InviteId, const string& Username, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }

    const char* sql = "UPDATE pending_actions SET status='rejected', responded_at=strftime('%s','now') WHERE id=? AND action_type='family_member_invite' AND target_user_id=? AND status='pending';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to reject invite"; return false; }
    sqlite3_bind_int(stmt, 1, InviteId);
    sqlite3_bind_int(stmt, 2, userId);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changes <= 0) { OutError = "Invite not found or already resolved"; return false; }
    return true;
}

bool LedgerManager::CancelMemberInvite(int InviteId, const string& Username, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }

    int familyId = 0;
    int createdByUserId = 0;
    {
        const char* querySql = "SELECT created_by_user_id,family_id FROM pending_actions WHERE id=? AND action_type='family_member_invite' AND status='pending' LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, querySql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to query invite"; return false; }
        sqlite3_bind_int(stmt, 1, InviteId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            createdByUserId = sqlite3_column_int(stmt, 0);
            familyId = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
    if (familyId <= 0) { OutError = "Invite not found or already resolved"; return false; }
    if (createdByUserId != userId && GetFamilyRoleNoLock(m_Db, familyId, userId) != "owner") {
        OutError = "Only inviter or family owner can cancel invite";
        return false;
    }

    const char* sql = "UPDATE pending_actions SET status='cancelled', responded_at=strftime('%s','now') WHERE id=? AND action_type='family_member_invite' AND status='pending';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) { OutError = "Failed to cancel invite"; return false; }
    sqlite3_bind_int(stmt, 1, InviteId);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changes <= 0) { OutError = "Invite not found or already resolved"; return false; }
    return true;
}

int LedgerManager::CleanupExpiredPendingActions(int64_t Now)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return 0;
    const char* sql = "UPDATE pending_actions SET status='expired', responded_at=? WHERE status='pending' AND expires_at>0 AND expires_at<=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(Now));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(Now));
    int rc = sqlite3_step(stmt);
    int changes = (rc == SQLITE_DONE) ? sqlite3_changes(m_Db) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

// ============================================================================
// 家庭组管理
// ============================================================================

bool LedgerManager::CreateFamily(const string& Name,
                                 const string& CreatorUsername,
                                 int& OutFamilyId,
                                 string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutFamilyId = 0;
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (Name.empty()) { OutError = "Family name is required"; return false; }

    const int userId = GetUserIdNoLock(m_Db, CreatorUsername);
    if (userId < 0) { OutError = "Creator user not found"; return false; }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError)
        != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin family creation transaction";
        sqlite3_free(txError);
        return false;
    }

    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutFamilyId = 0;
        OutError = Error;
        return false;
    };

    sqlite3_stmt* stmt = nullptr;
    const char* familySql = "INSERT INTO families (name, created_by) VALUES (?, ?);";
    if (sqlite3_prepare_v2(m_Db, familySql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare family creation");
    }
    sqlite3_bind_text(stmt, 1, Name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, userId);
    const int familyRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (familyRc != SQLITE_DONE) {
        return rollback("Failed to create family");
    }
    OutFamilyId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));

    // 家庭创建者和 owner 成员关系必须原子落库，禁止产生无 owner 的孤立家庭。
    const char* memberSql =
        "INSERT INTO family_members (family_id, user_id, role) VALUES (?, ?, 'owner');";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, memberSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare family owner creation");
    }
    sqlite3_bind_int(stmt, 1, OutFamilyId);
    sqlite3_bind_int(stmt, 2, userId);
    const int memberRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (memberRc != SQLITE_DONE) {
        return rollback("Failed to create family owner");
    }

    const char* contextSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=NULL WHERE id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare family context update");
    }
    sqlite3_bind_int(stmt, 1, OutFamilyId);
    sqlite3_bind_int(stmt, 2, userId);
    const int contextRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (contextRc != SQLITE_DONE) {
        return rollback("Failed to update current family context");
    }

    txError = nullptr;
    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit family creation";
        sqlite3_free(txError);
        return rollback(error);
    }

    printf("[Ledger] Family '%s' created (id=%d) by '%s'\n",
           Name.c_str(), OutFamilyId, CreatorUsername.c_str());
    return true;
}

bool LedgerManager::GetUserFamilies(const string& Username, string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    if (!m_Db) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) return true;

    int currentFamilyId = 0;
    const char* currentSql =
        "SELECT u.current_family_id FROM users u "
        "JOIN family_members fm ON fm.user_id=u.id AND fm.family_id=u.current_family_id "
        "WHERE u.id=? AND u.current_family_id IS NOT NULL LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, currentSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            currentFamilyId = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);

    struct FFamilyRow
    {
        int Id = 0;
        string Name;
        string Role;
        string CreatedAt;
    };
    vector<FFamilyRow> rows;
    const char* listSql =
        "SELECT f.id, f.name, fm.role, f.created_at "
        "FROM families f JOIN family_members fm ON fm.family_id=f.id "
        "WHERE fm.user_id=? ORDER BY f.id;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, listSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, userId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FFamilyRow row;
        row.Id = sqlite3_column_int(stmt, 0);
        row.Name = SafeCStr((const char*)sqlite3_column_text(stmt, 1), "");
        row.Role = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "member");
        row.CreatedAt = SafeCStr((const char*)sqlite3_column_text(stmt, 3), "");
        rows.push_back(row);
    }
    sqlite3_finalize(stmt);

    // 当前家庭仅是 UI 上下文；失效时修复到首个仍有成员关系的家庭。
    if (currentFamilyId <= 0 && !rows.empty()) {
        currentFamilyId = rows.front().Id;
        const char* updateSql =
            "UPDATE users SET current_family_id=?, current_ledger_id=NULL WHERE id=?;";
        stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, currentFamilyId);
            sqlite3_bind_int(stmt, 2, userId);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    ostringstream json;
    json << "[";
    for (size_t index = 0; index < rows.size(); ++index) {
        if (index > 0) json << ",";
        const FFamilyRow& row = rows[index];
        json << "{"
             << "\"id\":" << row.Id << ","
             << "\"name\":\"" << EscapeJsonString(row.Name) << "\","
             << "\"role\":\"" << EscapeJsonString(row.Role) << "\","
             << "\"createdAt\":\"" << EscapeJsonString(row.CreatedAt) << "\","
             << "\"isCurrent\":" << (row.Id == currentFamilyId ? "true" : "false")
             << "}";
    }
    json << "]";
    OutJson = json.str();
    return true;
}

bool LedgerManager::CreateLedger(int FamilyId,
                                 const string& Name,
                                 const string& CreatorUsername,
                                 int& OutLedgerId,
                                 string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutLedgerId = 0;
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }
    if (Name.empty()) { OutError = "Ledger name is required"; return false; }

    const int userId = GetUserIdNoLock(m_Db, CreatorUsername);
    if (userId < 0) { OutError = "Creator user not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, userId) != "owner") {
        OutError = "Only family owner can create ledger";
        return false;
    }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError)
        != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin ledger creation transaction";
        sqlite3_free(txError);
        return false;
    }

    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutLedgerId = 0;
        OutError = Error;
        return false;
    };

    const char* ledgerSql =
        "INSERT INTO ledgers (family_id, name, created_by) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, ledgerSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare ledger creation");
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    sqlite3_bind_text(stmt, 2, Name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, userId);
    const int ledgerRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (ledgerRc != SQLITE_DONE) {
        return rollback(sqlite3_errmsg(m_Db));
    }
    OutLedgerId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));

    // 默认分类严格归属账本，并与账本创建处于同一事务。
    if (!CreateDefaultCategories(OutLedgerId)) {
        return rollback("Failed to create ledger default categories");
    }

    const char* contextSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare ledger context update");
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    sqlite3_bind_int(stmt, 2, OutLedgerId);
    sqlite3_bind_int(stmt, 3, userId);
    const int contextRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (contextRc != SQLITE_DONE) {
        return rollback("Failed to update current ledger context");
    }

    txError = nullptr;
    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit ledger creation";
        sqlite3_free(txError);
        return rollback(error);
    }

    printf("[Ledger] Ledger '%s' created (id=%d, familyId=%d) by '%s'\n",
           Name.c_str(), OutLedgerId, FamilyId, CreatorUsername.c_str());
    return true;
}

bool LedgerManager::GetFamilyLedgers(int FamilyId,
                                     const string& Username,
                                     string& OutJson,
                                     string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, userId).empty()) {
        OutError = "Current user is not a family member";
        return false;
    }

    int currentLedgerId = 0;
    const char* currentSql =
        "SELECT u.current_ledger_id FROM users u "
        "JOIN ledgers l ON l.id=u.current_ledger_id AND l.family_id=? "
        "WHERE u.id=? AND u.current_family_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, currentSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, FamilyId);
        sqlite3_bind_int(stmt, 2, userId);
        sqlite3_bind_int(stmt, 3, FamilyId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            currentLedgerId = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);

    struct FLedgerRow
    {
        int Id = 0;
        string Name;
        string CreatedAt;
    };
    vector<FLedgerRow> rows;
    const char* listSql =
        "SELECT id, name, created_at FROM ledgers WHERE family_id=? ORDER BY id;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, listSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query family ledgers";
        return false;
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FLedgerRow row;
        row.Id = sqlite3_column_int(stmt, 0);
        row.Name = SafeCStr((const char*)sqlite3_column_text(stmt, 1), "");
        row.CreatedAt = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "");
        rows.push_back(row);
    }
    sqlite3_finalize(stmt);

    if (currentLedgerId <= 0 && !rows.empty()) {
        currentLedgerId = rows.front().Id;
        const char* updateSql =
            "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
        stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, FamilyId);
            sqlite3_bind_int(stmt, 2, currentLedgerId);
            sqlite3_bind_int(stmt, 3, userId);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    const bool bCanManage = GetFamilyRoleNoLock(m_Db, FamilyId, userId) == "owner";
    ostringstream json;
    json << "[";
    for (size_t index = 0; index < rows.size(); ++index) {
        if (index > 0) json << ",";
        const FLedgerRow& row = rows[index];
        json << "{"
             << "\"id\":" << row.Id << ","
             << "\"familyId\":" << FamilyId << ","
             << "\"name\":\"" << EscapeJsonString(row.Name) << "\","
             << "\"createdAt\":\"" << EscapeJsonString(row.CreatedAt) << "\","
             << "\"isCurrent\":" << (row.Id == currentLedgerId ? "true" : "false") << ","
             << "\"canManage\":" << (bCanManage ? "true" : "false")
             << "}";
    }
    json << "]";
    OutJson = json.str();
    return true;
}

bool LedgerManager::GetCurrentFamilyId(const string& Username, int& OutFamilyId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutFamilyId = 0;
    if (!m_Db) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) return false;

    const char* currentSql =
        "SELECT u.current_family_id FROM users u "
        "JOIN family_members fm ON fm.family_id=u.current_family_id AND fm.user_id=u.id "
        "WHERE u.id=? AND u.current_family_id IS NOT NULL LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, currentSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            OutFamilyId = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    if (OutFamilyId > 0) return true;

    const char* fallbackSql =
        "SELECT family_id FROM family_members WHERE user_id=? ORDER BY family_id LIMIT 1;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, fallbackSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, userId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OutFamilyId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (OutFamilyId <= 0) return false;

    int fallbackLedgerId = 0;
    const char* ledgerSql = "SELECT id FROM ledgers WHERE family_id=? ORDER BY id LIMIT 1;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, ledgerSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, OutFamilyId);
        if (sqlite3_step(stmt) == SQLITE_ROW) fallbackLedgerId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    const char* updateSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, OutFamilyId);
    if (fallbackLedgerId > 0) sqlite3_bind_int(stmt, 2, fallbackLedgerId);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, userId);
    const bool bUpdated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return bUpdated;
}

bool LedgerManager::SetCurrentFamilyId(const string& Username,
                                       int FamilyId,
                                       string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, userId).empty()) {
        OutError = "Current user is not a family member";
        return false;
    }

    int ledgerId = 0;
    const char* ledgerSql = "SELECT id FROM ledgers WHERE family_id=? ORDER BY id LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, ledgerSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query family ledger";
        return false;
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    if (sqlite3_step(stmt) == SQLITE_ROW) ledgerId = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char* updateSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare family context update";
        return false;
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    if (ledgerId > 0) sqlite3_bind_int(stmt, 2, ledgerId);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, userId);
    const bool bUpdated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!bUpdated) OutError = "Failed to update current family";
    return bUpdated;
}

bool LedgerManager::GetCurrentLedgerId(const string& Username, int& OutLedgerId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutLedgerId = 0;
    if (!m_Db) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) return false;

    const char* currentSql =
        "SELECT u.current_ledger_id FROM users u "
        "JOIN ledgers l ON l.id=u.current_ledger_id "
        "JOIN family_members fm ON fm.family_id=l.family_id AND fm.user_id=u.id "
        "WHERE u.id=? AND u.current_ledger_id IS NOT NULL LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, currentSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            OutLedgerId = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    if (OutLedgerId > 0) return true;

    int familyId = 0;
    const char* fallbackSql =
        "SELECT l.id, l.family_id FROM ledgers l "
        "JOIN family_members fm ON fm.family_id=l.family_id "
        "WHERE fm.user_id=? ORDER BY l.family_id, l.id LIMIT 1;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, fallbackSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, userId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OutLedgerId = sqlite3_column_int(stmt, 0);
        familyId = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (OutLedgerId <= 0 || familyId <= 0) return false;

    const char* updateSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_int(stmt, 2, OutLedgerId);
    sqlite3_bind_int(stmt, 3, userId);
    const bool bUpdated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return bUpdated;
}

bool LedgerManager::SetCurrentLedgerId(const string& Username,
                                       int LedgerId,
                                       string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0) { OutError = "Missing ledgerId"; return false; }

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }
    const int familyId = GetLedgerFamilyIdNoLock(m_Db, LedgerId);
    if (familyId <= 0) { OutError = "Ledger not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, familyId, userId).empty()) {
        OutError = "Current user cannot access ledger";
        return false;
    }

    const char* updateSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare ledger context update";
        return false;
    }
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_int(stmt, 2, LedgerId);
    sqlite3_bind_int(stmt, 3, userId);
    const bool bUpdated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!bUpdated) OutError = "Failed to update current ledger";
    return bUpdated;
}

bool LedgerManager::IsFamilyMember(int FamilyId, const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || FamilyId <= 0) return false;
    const int userId = GetUserIdNoLock(m_Db, Username);
    return userId > 0 && !GetFamilyRoleNoLock(m_Db, FamilyId, userId).empty();
}

bool LedgerManager::IsFamilyOwner(int FamilyId, const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || FamilyId <= 0) return false;
    const int userId = GetUserIdNoLock(m_Db, Username);
    return userId > 0 && GetFamilyRoleNoLock(m_Db, FamilyId, userId) == "owner";
}

bool LedgerManager::GetLedgerFamilyId(int LedgerId, int& OutFamilyId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutFamilyId = 0;
    if (!m_Db || LedgerId <= 0) return false;

    OutFamilyId = GetLedgerFamilyIdNoLock(m_Db, LedgerId);
    return OutFamilyId > 0;
}

bool LedgerManager::CanAccessLedger(int LedgerId, const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || LedgerId <= 0) return false;
    const int userId = GetUserIdNoLock(m_Db, Username);
    return userId > 0 && CanAccessLedgerNoLock(m_Db, LedgerId, userId);
}

bool LedgerManager::CanManageLedger(int LedgerId, const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || LedgerId <= 0) return false;
    const int userId = GetUserIdNoLock(m_Db, Username);
    return userId > 0 && CanManageLedgerNoLock(m_Db, LedgerId, userId);
}

bool LedgerManager::AddFamilyMember(int FamilyId,
                                    const string& Username,
                                    const string& Role,
                                    const string& RequestedByUsername,
                                    string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }
    if (!IsValidFamilyRole(Role)) { OutError = "Family role must be owner or member"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    const int targetUserId = GetUserIdNoLock(m_Db, Username);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (targetUserId < 0) { OutError = "Target user not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, requesterId) != "owner") {
        OutError = "Only family owner can add member";
        return false;
    }
    if (!GetFamilyRoleNoLock(m_Db, FamilyId, targetUserId).empty()) {
        OutError = "User is already a family member";
        return false;
    }

    const char* sql =
        "INSERT INTO family_members (family_id, user_id, role) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare family member creation";
        return false;
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    sqlite3_bind_int(stmt, 2, targetUserId);
    sqlite3_bind_text(stmt, 3, Role.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to add family member";
        return false;
    }
    return true;
}

bool LedgerManager::UpdateFamilyMemberRole(int FamilyId,
                                           const string& Username,
                                           const string& Role,
                                           const string& RequestedByUsername,
                                           string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }
    if (!IsValidFamilyRole(Role)) { OutError = "Family role must be owner or member"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    const int targetUserId = GetUserIdNoLock(m_Db, Username);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (targetUserId < 0) { OutError = "Target user not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, requesterId) != "owner") {
        OutError = "Only family owner can change member role";
        return false;
    }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError)
        != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin role update transaction";
        sqlite3_free(txError);
        return false;
    }
    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = Error;
        return false;
    };

    const string currentRole = GetFamilyRoleNoLock(m_Db, FamilyId, targetUserId);
    if (currentRole.empty()) return rollback("Target user is not a family member");
    if (currentRole == Role) {
        sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, nullptr);
        return true;
    }

    // 降级 owner 前在写事务内重新计数，禁止并发移除最后一个 owner。
    if (currentRole == "owner" && Role == "member") {
        const char* countSql =
            "SELECT COUNT(*) FROM family_members WHERE family_id=? AND role='owner';";
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, countSql, -1, &countStmt, nullptr) != SQLITE_OK) {
            return rollback("Failed to count family owners");
        }
        sqlite3_bind_int(countStmt, 1, FamilyId);
        int ownerCount = 0;
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            ownerCount = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
        if (ownerCount <= 1) {
            return rollback("Family must retain at least one owner");
        }
    }

    const char* updateSql =
        "UPDATE family_members SET role=? WHERE family_id=? AND user_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare family role update");
    }
    sqlite3_bind_text(stmt, 1, Role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, FamilyId);
    sqlite3_bind_int(stmt, 3, targetUserId);
    const int rc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changes <= 0) {
        return rollback("Failed to update family member role");
    }

    txError = nullptr;
    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit family role update";
        sqlite3_free(txError);
        return rollback(error);
    }
    return true;
}

bool LedgerManager::RemoveFamilyMember(int FamilyId,
                                       const string& Username,
                                       const string& RequestedByUsername,
                                       string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    const int targetUserId = GetUserIdNoLock(m_Db, Username);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (targetUserId < 0) { OutError = "Target user not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, requesterId) != "owner") {
        OutError = "Only family owner can remove member";
        return false;
    }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError)
        != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin member removal transaction";
        sqlite3_free(txError);
        return false;
    }
    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = Error;
        return false;
    };

    const string targetRole = GetFamilyRoleNoLock(m_Db, FamilyId, targetUserId);
    if (targetRole.empty()) return rollback("Target user is not a family member");

    if (targetRole == "owner") {
        const char* countSql =
            "SELECT COUNT(*) FROM family_members WHERE family_id=? AND role='owner';";
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, countSql, -1, &countStmt, nullptr) != SQLITE_OK) {
            return rollback("Failed to count family owners");
        }
        sqlite3_bind_int(countStmt, 1, FamilyId);
        int ownerCount = 0;
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            ownerCount = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
        if (ownerCount <= 1) {
            return rollback("Family must retain at least one owner");
        }
    }

    const char* deleteSql =
        "DELETE FROM family_members WHERE family_id=? AND user_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, deleteSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare family member removal");
    }
    sqlite3_bind_int(stmt, 1, FamilyId);
    sqlite3_bind_int(stmt, 2, targetUserId);
    const int deleteRc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (deleteRc != SQLITE_DONE || changes <= 0) {
        return rollback("Failed to remove family member");
    }

    // 上下文不参与授权，但必须清除已失效引用，避免前端继续展示不可访问实体。
    const char* contextSql =
        "UPDATE users SET current_family_id=NULL, current_ledger_id=NULL "
        "WHERE id=? AND current_family_id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare removed member context cleanup");
    }
    sqlite3_bind_int(stmt, 1, targetUserId);
    sqlite3_bind_int(stmt, 2, FamilyId);
    const int contextRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (contextRc != SQLITE_DONE) {
        return rollback("Failed to cleanup removed member context");
    }

    txError = nullptr;
    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit member removal";
        sqlite3_free(txError);
        return rollback(error);
    }
    return true;
}

bool LedgerManager::GetFamilyMembers(int FamilyId,
                                     const string& RequestedByUsername,
                                     string& OutJson,
                                     string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (FamilyId <= 0) { OutError = "Missing familyId"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, FamilyId, requesterId).empty()) {
        OutError = "Current user is not a family member";
        return false;
    }

    const char* sql =
        "SELECT u.id, u.username, fm.role, fm.created_at "
        "FROM family_members fm JOIN users u ON u.id=fm.user_id "
        "WHERE fm.family_id=? ORDER BY CASE fm.role WHEN 'owner' THEN 0 ELSE 1 END, u.username;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query family members";
        return false;
    }
    sqlite3_bind_int(stmt, 1, FamilyId);

    ostringstream json;
    json << "[";
    bool bFirst = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!bFirst) json << ",";
        bFirst = false;
        json << "{"
             << "\"userId\":" << sqlite3_column_int(stmt, 0) << ","
             << "\"username\":\"" << EscapeJsonString(
                    SafeCStr((const char*)sqlite3_column_text(stmt, 1), "")) << "\","
             << "\"role\":\"" << EscapeJsonString(
                    SafeCStr((const char*)sqlite3_column_text(stmt, 2), "member")) << "\","
             << "\"createdAt\":\"" << EscapeJsonString(
                    SafeCStr((const char*)sqlite3_column_text(stmt, 3), "")) << "\""
             << "}";
    }
    sqlite3_finalize(stmt);
    json << "]";
    OutJson = json.str();
    return true;
}

bool LedgerManager::CreateGroup(const string& Name, const string& CreatorUsername,
                                 int& OutGroupId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutGroupId = 0;
    if (!m_Db || Name.empty()) return false;

    const int userId = GetUserIdNoLock(m_Db, CreatorUsername);
    if (userId < 0) return false;

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError)
        != SQLITE_OK) {
        sqlite3_free(txError);
        return false;
    }

    auto rollback = [&]() -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutGroupId = 0;
        return false;
    };

    sqlite3_stmt* stmt = nullptr;
    const char* familySql = "INSERT INTO families (name, created_by) VALUES (?, ?);";
    if (sqlite3_prepare_v2(m_Db, familySql, -1, &stmt, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_text(stmt, 1, Name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, userId);
    const int familyRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (familyRc != SQLITE_DONE) return rollback();
    const int familyId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));

    const char* memberSql =
        "INSERT INTO family_members (family_id, user_id, role) VALUES (?, ?, 'owner');";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, memberSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_int(stmt, 2, userId);
    const int memberRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (memberRc != SQLITE_DONE) return rollback();

    // 旧 group.create 对前端代表“创建可立即记账的空间”，因此兼容层原子创建同名默认账本。
    const char* ledgerSql =
        "INSERT INTO ledgers (family_id, name, created_by) VALUES (?, ?, ?);";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, ledgerSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_text(stmt, 2, Name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, userId);
    const int ledgerRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (ledgerRc != SQLITE_DONE) return rollback();
    OutGroupId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));

    if (!CreateDefaultCategories(OutGroupId)) return rollback();

    const char* contextSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, contextSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_int(stmt, 2, OutGroupId);
    sqlite3_bind_int(stmt, 3, userId);
    const int contextRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (contextRc != SQLITE_DONE) return rollback();

    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        sqlite3_free(txError);
        return rollback();
    }

    printf("[Ledger] Compatibility group '%s' created (familyId=%d, ledgerId=%d) by '%s'\n",
           Name.c_str(), familyId, OutGroupId, CreatorUsername.c_str());
    return true;
}

bool LedgerManager::GetUserGroups(const string& Username, string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    if (!m_Db) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) return true;

    int currentLedgerId = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* currentSql =
        "SELECT u.current_ledger_id FROM users u "
        "JOIN ledgers l ON l.id=u.current_ledger_id "
        "JOIN family_members fm ON fm.family_id=l.family_id AND fm.user_id=u.id "
        "WHERE u.id=? AND u.current_ledger_id IS NOT NULL LIMIT 1;";
    if (sqlite3_prepare_v2(m_Db, currentSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        if (sqlite3_step(stmt) == SQLITE_ROW) currentLedgerId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    struct FGroupCompatibilityRow
    {
        int LedgerId = 0;
        int FamilyId = 0;
        string Name;
        string FamilyName;
        string Role;
        string CreatedAt;
    };
    vector<FGroupCompatibilityRow> rows;
    const char* listSql =
        "SELECT l.id, f.id, l.name, f.name, fm.role, l.created_at "
        "FROM family_members fm "
        "JOIN families f ON f.id=fm.family_id "
        "JOIN ledgers l ON l.family_id=f.id "
        "WHERE fm.user_id=? ORDER BY f.id, l.id;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, listSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, userId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FGroupCompatibilityRow row;
        row.LedgerId = sqlite3_column_int(stmt, 0);
        row.FamilyId = sqlite3_column_int(stmt, 1);
        row.Name = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "");
        row.FamilyName = SafeCStr((const char*)sqlite3_column_text(stmt, 3), "");
        row.Role = SafeCStr((const char*)sqlite3_column_text(stmt, 4), "member");
        row.CreatedAt = SafeCStr((const char*)sqlite3_column_text(stmt, 5), "");
        rows.push_back(row);
    }
    sqlite3_finalize(stmt);

    if (currentLedgerId <= 0 && !rows.empty()) {
        currentLedgerId = rows.front().LedgerId;
        const char* updateSql =
            "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE id=?;";
        stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, rows.front().FamilyId);
        sqlite3_bind_int(stmt, 2, currentLedgerId);
        sqlite3_bind_int(stmt, 3, userId);
        const int updateRc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (updateRc != SQLITE_DONE) return false;
    }

    ostringstream arr;
    arr << "[";
    for (size_t index = 0; index < rows.size(); ++index) {
        if (index > 0) arr << ",";
        const FGroupCompatibilityRow& row = rows[index];
        arr << "{"
            << "\"id\":" << row.LedgerId << ","
            << "\"familyId\":" << row.FamilyId << ","
            << "\"name\":\"" << EscapeJsonString(row.Name) << "\","
            << "\"familyName\":\"" << EscapeJsonString(row.FamilyName) << "\","
            << "\"role\":\"" << EscapeJsonString(row.Role) << "\","
            << "\"createdAt\":\"" << EscapeJsonString(row.CreatedAt) << "\","
            << "\"isCurrent\":" << (row.LedgerId == currentLedgerId ? "true" : "false")
            << "}";
    }
    arr << "]";
    OutJson = arr.str();
    return true;
}

bool LedgerManager::DeleteGroup(int GroupId, const string& Username,
                                string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (GroupId <= 0) { OutError = "Missing groupId"; return false; }

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }

    const int familyId = GetLedgerFamilyIdNoLock(m_Db, GroupId);
    if (familyId <= 0) { OutError = "Ledger not found"; return false; }
    if (GetFamilyRoleNoLock(m_Db, familyId, userId) != "owner") {
        OutError = "Only family owner can delete this ledger";
        return false;
    }

    char* errMsg = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        OutError = errMsg ? errMsg : "Failed to begin transaction";
        sqlite3_free(errMsg);
        return false;
    }

    auto rollbackWithError = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = Error;
        return false;
    };

    // 优先回退到同一家庭的其他账本；家庭已无其他账本时仅清空账本上下文，保留家庭上下文。
    int fallbackLedgerId = 0;
    const char* fallbackSql =
        "SELECT id FROM ledgers WHERE family_id=? AND id<>? ORDER BY id LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, fallbackSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollbackWithError("Failed to prepare fallback ledger query");
    }
    sqlite3_bind_int(stmt, 1, familyId);
    sqlite3_bind_int(stmt, 2, GroupId);
    if (sqlite3_step(stmt) == SQLITE_ROW) fallbackLedgerId = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char* repairContextSql =
        "UPDATE users SET current_family_id=?, current_ledger_id=? WHERE current_ledger_id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, repairContextSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollbackWithError("Failed to prepare ledger context repair");
    }
    sqlite3_bind_int(stmt, 1, familyId);
    if (fallbackLedgerId > 0) sqlite3_bind_int(stmt, 2, fallbackLedgerId);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, GroupId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return rollbackWithError("Failed to repair ledger context");

    // V2 外键负责级联删除分类、流水和导入任务，避免手工维护重复且易漂移的清理 SQL。
    const char* deleteSql = "DELETE FROM ledgers WHERE id=? AND family_id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, deleteSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollbackWithError("Failed to prepare ledger delete");
    }
    sqlite3_bind_int(stmt, 1, GroupId);
    sqlite3_bind_int(stmt, 2, familyId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(m_Db) <= 0) {
        return rollbackWithError("Failed to delete ledger");
    }

    errMsg = nullptr;
    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        OutError = errMsg ? errMsg : "Failed to commit ledger delete";
        sqlite3_free(errMsg);
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    printf("[Ledger] Compatibility ledger id=%d deleted by '%s'\n", GroupId, Username.c_str());
    return true;
}

bool LedgerManager::GetCurrentGroupId(const string& Username, int& OutGroupId)
{
    // 兼容接口中的 groupId 已统一映射为 V2 账本 ID。
    return GetCurrentLedgerId(Username, OutGroupId);
}

bool LedgerManager::SetCurrentGroupId(const string& Username, int GroupId,
                                      string& OutError)
{
    // 兼容接口中的 groupId 已统一映射为 V2 账本 ID，授权由账本所属家庭成员关系判定。
    return SetCurrentLedgerId(Username, GroupId, OutError);
}

bool LedgerManager::GetGroupMembers(int GroupId, string& OutJson)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    if (!m_Db || GroupId <= 0) return false;

    // 兼容接口中的 groupId 已代表账本 ID；成员关系归属于账本所在家庭，
    // 因此必须沿 ledgers.family_id 查询 family_members，不能继续读取旧 group_members。
    const char* sql =
        "SELECT u.id, u.username, fm.role "
        "FROM ledgers l "
        "JOIN family_members fm ON fm.family_id=l.family_id "
        "JOIN users u ON u.id=fm.user_id "
        "WHERE l.id=? "
        "ORDER BY CASE fm.role WHEN 'owner' THEN 0 ELSE 1 END, u.username;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, GroupId);

    ostringstream arr;
    arr << "[";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) arr << ",";
        first = false;
        arr << "{"
            << "\"userId\":" << sqlite3_column_int(stmt, 0) << ","
            << "\"username\":\"" << EscapeJsonString(
                   SafeCStr((const char*)sqlite3_column_text(stmt, 1), "")) << "\","
            << "\"role\":\"" << EscapeJsonString(
                   SafeCStr((const char*)sqlite3_column_text(stmt, 2), "member")) << "\""
            << "}";
    }
    const int queryRc = sqlite3_errcode(m_Db);
    sqlite3_finalize(stmt);
    if (queryRc != SQLITE_OK && queryRc != SQLITE_DONE && queryRc != SQLITE_ROW) return false;

    arr << "]";
    OutJson = arr.str();
    return true;
}

// ============================================================================
// 分类管理
// ============================================================================

bool LedgerManager::CreateLedgerCategory(int LedgerId,
                                         const string& Name,
                                         const string& Type,
                                         int ParentId,
                                         int SortOrder,
                                         const string& RequestedByUsername,
                                         int& OutId,
                                         string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutId = 0;
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0) { OutError = "Missing ledgerId"; return false; }
    if (Name.empty()) { OutError = "Category name is required"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (!CanManageLedgerNoLock(m_Db, LedgerId, requesterId)) {
        OutError = "Only family owner can manage ledger categories";
        return false;
    }

    int normalizedParentId = ParentId > 0 ? ParentId : 0;
    string normalizedType = Type.empty() ? "expense" : Type;
    if (normalizedType != "expense" && normalizedType != "income") {
        OutError = "Category type must be expense or income";
        return false;
    }

    if (normalizedParentId > 0) {
        const char* parentSql =
            "SELECT type FROM account_categories "
            "WHERE id=? AND ledger_id=? AND parent_id=0;";
        sqlite3_stmt* parentStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, parentSql, -1, &parentStmt, nullptr) != SQLITE_OK) {
            OutError = "Failed to validate parent category";
            return false;
        }
        sqlite3_bind_int(parentStmt, 1, normalizedParentId);
        sqlite3_bind_int(parentStmt, 2, LedgerId);
        if (sqlite3_step(parentStmt) == SQLITE_ROW) {
            normalizedType = SafeCStr(
                (const char*)sqlite3_column_text(parentStmt, 0), normalizedType.c_str());
        } else {
            sqlite3_finalize(parentStmt);
            OutError = "Parent category must be a top-level category in the same ledger";
            return false;
        }
        sqlite3_finalize(parentStmt);
    }

    const char* sql =
        "INSERT INTO account_categories "
        "(ledger_id, parent_id, name, type, sort_order, is_system) "
        "VALUES (?, ?, ?, ?, ?, 0);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare category creation";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_int(stmt, 2, normalizedParentId);
    sqlite3_bind_text(stmt, 3, Name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, normalizedType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, SortOrder);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to create category";
        return false;
    }

    OutId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
    return true;
}

bool LedgerManager::GetLedgerCategories(int LedgerId,
                                        const string& Type,
                                        const string& RequestedByUsername,
                                        string& OutJson,
                                        string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0) { OutError = "Missing ledgerId"; return false; }
    if (!Type.empty() && Type != "expense" && Type != "income") {
        OutError = "Category type must be expense or income";
        return false;
    }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (!CanAccessLedgerNoLock(m_Db, LedgerId, requesterId)) {
        OutError = "Current user cannot access this ledger";
        return false;
    }

    const char* sql =
        "SELECT c.id, c.name, c.type, c.parent_id, c.sort_order, c.is_system, "
        "       COALESCE(p.name, '') "
        "FROM account_categories c "
        "LEFT JOIN account_categories p "
        "  ON p.id=c.parent_id AND p.ledger_id=c.ledger_id "
        "WHERE c.ledger_id=? AND (?='' OR c.type=?) "
        "ORDER BY c.type, CASE WHEN c.parent_id=0 THEN c.id ELSE c.parent_id END, "
        "         c.parent_id, c.sort_order, c.id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query ledger categories";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_text(stmt, 2, Type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, Type.c_str(), -1, SQLITE_TRANSIENT);

    ostringstream json;
    json << "[";
    bool bFirst = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!bFirst) json << ",";
        bFirst = false;
        const int parentId = sqlite3_column_int(stmt, 3);
        json << "{"
             << "\"id\":" << sqlite3_column_int(stmt, 0) << ","
             << "\"ledgerId\":" << LedgerId << ","
             << "\"name\":\"" << EscapeJsonString(
                    SafeCStr((const char*)sqlite3_column_text(stmt, 1), "")) << "\","
             << "\"type\":\"" << EscapeJsonString(
                    SafeCStr((const char*)sqlite3_column_text(stmt, 2), "expense")) << "\","
             << "\"parentId\":" << parentId << ","
             << "\"level\":" << (parentId > 0 ? 2 : 1) << ","
             << "\"sortOrder\":" << sqlite3_column_int(stmt, 4) << ","
             << "\"isSystem\":" << (sqlite3_column_int(stmt, 5) != 0 ? "true" : "false") << ","
             << "\"parentName\":\"" << EscapeJsonString(
                    SafeCStr((const char*)sqlite3_column_text(stmt, 6), "")) << "\""
             << "}";
    }
    sqlite3_finalize(stmt);
    json << "]";
    OutJson = json.str();
    return true;
}

bool LedgerManager::GetCategoryLedgerId(int CategoryId, int& OutLedgerId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutLedgerId = 0;
    if (!m_Db || CategoryId <= 0) return false;

    const char* sql = "SELECT ledger_id FROM account_categories WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, CategoryId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OutLedgerId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return OutLedgerId > 0;
}

bool LedgerManager::UpdateLedgerCategory(int CategoryId,
                                         const string& Name,
                                         const string& Type,
                                         int ParentId,
                                         int SortOrder,
                                         const string& RequestedByUsername,
                                         string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (CategoryId <= 0) { OutError = "Missing categoryId"; return false; }
    if (Name.empty()) { OutError = "Category name is required"; return false; }

    int ledgerId = 0;
    string currentType;
    const char* categorySql =
        "SELECT ledger_id, type FROM account_categories WHERE id=?;";
    sqlite3_stmt* categoryStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, categorySql, -1, &categoryStmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query category";
        return false;
    }
    sqlite3_bind_int(categoryStmt, 1, CategoryId);
    if (sqlite3_step(categoryStmt) == SQLITE_ROW) {
        ledgerId = sqlite3_column_int(categoryStmt, 0);
        currentType = SafeCStr(
            (const char*)sqlite3_column_text(categoryStmt, 1), "expense");
    }
    sqlite3_finalize(categoryStmt);
    if (ledgerId <= 0) { OutError = "Category not found"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (!CanManageLedgerNoLock(m_Db, ledgerId, requesterId)) {
        OutError = "Only family owner can manage ledger categories";
        return false;
    }

    const int normalizedParentId = ParentId > 0 ? ParentId : 0;
    string normalizedType = Type.empty() ? currentType : Type;
    if (normalizedType != "expense" && normalizedType != "income") {
        OutError = "Category type must be expense or income";
        return false;
    }
    if (normalizedParentId == CategoryId) {
        OutError = "Category cannot be its own parent";
        return false;
    }

    if (normalizedParentId > 0) {
        const char* childSql =
            "SELECT COUNT(*) FROM account_categories WHERE parent_id=? AND ledger_id=?;";
        sqlite3_stmt* childStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, childSql, -1, &childStmt, nullptr) != SQLITE_OK) {
            OutError = "Failed to validate category children";
            return false;
        }
        sqlite3_bind_int(childStmt, 1, CategoryId);
        sqlite3_bind_int(childStmt, 2, ledgerId);
        int childCount = 0;
        if (sqlite3_step(childStmt) == SQLITE_ROW) {
            childCount = sqlite3_column_int(childStmt, 0);
        }
        sqlite3_finalize(childStmt);
        if (childCount > 0) {
            OutError = "Category with children cannot become a child category";
            return false;
        }

        const char* parentSql =
            "SELECT type FROM account_categories "
            "WHERE id=? AND ledger_id=? AND parent_id=0;";
        sqlite3_stmt* parentStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, parentSql, -1, &parentStmt, nullptr) != SQLITE_OK) {
            OutError = "Failed to validate parent category";
            return false;
        }
        sqlite3_bind_int(parentStmt, 1, normalizedParentId);
        sqlite3_bind_int(parentStmt, 2, ledgerId);
        if (sqlite3_step(parentStmt) == SQLITE_ROW) {
            normalizedType = SafeCStr(
                (const char*)sqlite3_column_text(parentStmt, 0), normalizedType.c_str());
        } else {
            sqlite3_finalize(parentStmt);
            OutError = "Parent category must be a top-level category in the same ledger";
            return false;
        }
        sqlite3_finalize(parentStmt);
    }

    const char* updateSql =
        "UPDATE account_categories "
        "SET name=?, type=?, parent_id=?, sort_order=? WHERE id=? AND ledger_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare category update";
        return false;
    }
    sqlite3_bind_text(stmt, 1, Name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, normalizedType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, normalizedParentId);
    sqlite3_bind_int(stmt, 4, SortOrder);
    sqlite3_bind_int(stmt, 5, CategoryId);
    sqlite3_bind_int(stmt, 6, ledgerId);
    const int rc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changes <= 0) {
        OutError = "Failed to update category";
        return false;
    }
    return true;
}

bool LedgerManager::DeleteLedgerCategory(int CategoryId,
                                         const string& RequestedByUsername,
                                         string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (CategoryId <= 0) { OutError = "Missing categoryId"; return false; }

    int ledgerId = 0;
    string categoryName;
    string categoryType;
    const char* categorySql =
        "SELECT ledger_id, name, type FROM account_categories WHERE id=?;";
    sqlite3_stmt* categoryStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, categorySql, -1, &categoryStmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to query category";
        return false;
    }
    sqlite3_bind_int(categoryStmt, 1, CategoryId);
    if (sqlite3_step(categoryStmt) == SQLITE_ROW) {
        ledgerId = sqlite3_column_int(categoryStmt, 0);
        categoryName = SafeCStr(
            (const char*)sqlite3_column_text(categoryStmt, 1), "");
        categoryType = SafeCStr(
            (const char*)sqlite3_column_text(categoryStmt, 2), "expense");
    }
    sqlite3_finalize(categoryStmt);
    if (ledgerId <= 0) { OutError = "Category not found"; return false; }

    const int requesterId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (requesterId < 0) { OutError = "Requester user not found"; return false; }
    if (!CanManageLedgerNoLock(m_Db, ledgerId, requesterId)) {
        OutError = "Only family owner can manage ledger categories";
        return false;
    }
    if (IsProtectedFallbackCategoryName(categoryName, categoryType)) {
        OutError = "Protected fallback category cannot be deleted";
        return false;
    }

    char* txError = nullptr;
    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &txError)
        != SQLITE_OK) {
        OutError = txError ? txError : "Failed to begin category deletion transaction";
        sqlite3_free(txError);
        return false;
    }
    auto rollback = [&](const string& Error) -> bool {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = Error;
        return false;
    };

    const char* childSql =
        "SELECT COUNT(*) FROM account_categories WHERE parent_id=? AND ledger_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, childSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to validate category children");
    }
    sqlite3_bind_int(stmt, 1, CategoryId);
    sqlite3_bind_int(stmt, 2, ledgerId);
    int childCount = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        childCount = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (childCount > 0) {
        return rollback("Category with children cannot be deleted");
    }

    auto findFallback = [&](const string& FallbackType,
                            const string& FallbackName) -> int {
        const char* sql =
            "SELECT id FROM account_categories "
            "WHERE ledger_id=? AND type=? AND name=? AND parent_id=0 "
            "ORDER BY is_system DESC, id LIMIT 1;";
        sqlite3_stmt* fallbackStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, sql, -1, &fallbackStmt, nullptr) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_int(fallbackStmt, 1, ledgerId);
        sqlite3_bind_text(fallbackStmt, 2, FallbackType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(fallbackStmt, 3, FallbackName.c_str(), -1, SQLITE_TRANSIENT);
        int fallbackId = 0;
        if (sqlite3_step(fallbackStmt) == SQLITE_ROW) {
            fallbackId = sqlite3_column_int(fallbackStmt, 0);
        }
        sqlite3_finalize(fallbackStmt);
        return fallbackId;
    };

    const int expenseFallbackId = findFallback("expense", "其他支出");
    const int incomeFallbackId = findFallback("income", "其他收入");
    if (expenseFallbackId <= 0 || incomeFallbackId <= 0) {
        return rollback("Ledger fallback categories are missing");
    }

    const char* migrateSql =
        "UPDATE transactions "
        "SET category_id=CASE WHEN type='income' THEN ? ELSE ? END "
        "WHERE ledger_id=? AND category_id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, migrateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare transaction category migration");
    }
    sqlite3_bind_int(stmt, 1, incomeFallbackId);
    sqlite3_bind_int(stmt, 2, expenseFallbackId);
    sqlite3_bind_int(stmt, 3, ledgerId);
    sqlite3_bind_int(stmt, 4, CategoryId);
    const int migrateRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (migrateRc != SQLITE_DONE) {
        return rollback("Failed to migrate transactions to fallback category");
    }

    const char* deleteSql =
        "DELETE FROM account_categories WHERE id=? AND ledger_id=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, deleteSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rollback("Failed to prepare category deletion");
    }
    sqlite3_bind_int(stmt, 1, CategoryId);
    sqlite3_bind_int(stmt, 2, ledgerId);
    const int deleteRc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(m_Db);
    sqlite3_finalize(stmt);
    if (deleteRc != SQLITE_DONE || changes <= 0) {
        return rollback("Failed to delete category");
    }

    txError = nullptr;
    if (sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, &txError) != SQLITE_OK) {
        const string error = txError ? txError : "Failed to commit category deletion";
        sqlite3_free(txError);
        return rollback(error);
    }
    return true;
}

// ============================================================================
// 账目管理
// ============================================================================

bool LedgerManager::CreateLedgerTransaction(int LedgerId,
                                             int CategoryId,
                                             double Amount,
                                             const string& Type,
                                             const string& Description,
                                             const string& CreatedByUsername,
                                             const string& Date,
                                             int& OutId,
                                             string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutId = 0;
    OutError.clear();

    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0 || CategoryId <= 0) { OutError = "Invalid ledger or category"; return false; }
    if (Amount <= 0.0) { OutError = "Transaction amount must be greater than zero"; return false; }
    if (Type != "expense" && Type != "income") { OutError = "Invalid transaction type"; return false; }
    if (Date.empty()) { OutError = "Transaction date is required"; return false; }

    const int userId = GetUserIdNoLock(m_Db, CreatedByUsername);
    if (userId <= 0) { OutError = "User not found"; return false; }
    if (!CanAccessLedgerNoLock(m_Db, LedgerId, userId)) {
        OutError = "Permission denied";
        return false;
    }

    // 分类和流水必须位于同一账本，并保持收支类型一致，防止跨账本外键穿透。
    const char* categorySql =
        "SELECT COUNT(*) FROM account_categories "
        "WHERE id=? AND ledger_id=? AND type=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, categorySql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to validate transaction category";
        return false;
    }
    sqlite3_bind_int(stmt, 1, CategoryId);
    sqlite3_bind_int(stmt, 2, LedgerId);
    sqlite3_bind_text(stmt, 3, Type.c_str(), -1, SQLITE_TRANSIENT);
    const bool categoryValid =
        sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    if (!categoryValid) {
        OutError = "Category does not belong to ledger or type mismatches";
        return false;
    }

    const char* insertSql =
        "INSERT INTO transactions "
        "(ledger_id, category_id, amount, type, description, created_by, transaction_date) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(m_Db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare transaction creation";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_int(stmt, 2, CategoryId);
    sqlite3_bind_double(stmt, 3, Amount);
    sqlite3_bind_text(stmt, 4, Type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, Description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, userId);
    sqlite3_bind_text(stmt, 7, Date.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to create transaction";
        return false;
    }

    OutId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
    return true;
}

bool LedgerManager::GetLedgerTransactions(int LedgerId,
                                           const string& RequestedByUsername,
                                           const string& DateFrom,
                                           const string& DateTo,
                                           int CategoryId,
                                           const string& Type,
                                           int Offset,
                                           int Limit,
                                           string& OutJson,
                                           int& OutTotal,
                                           string& OutError,
                                           int ParentCategoryId,
                                           const string& SortBy,
                                           const string& SortOrder)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "[]";
    OutTotal = 0;
    OutError.clear();

    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0) { OutError = "Invalid ledger"; return false; }
    if (!Type.empty() && Type != "expense" && Type != "income") {
        OutError = "Invalid transaction type filter";
        return false;
    }

    const int userId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (userId <= 0) { OutError = "User not found"; return false; }
    if (!CanAccessLedgerNoLock(m_Db, LedgerId, userId)) {
        OutError = "Permission denied";
        return false;
    }

    // 分类过滤本身也必须处于当前账本，避免利用其他账本分类 ID 观察数据差异。
    const int filterCategoryId = CategoryId > 0 ? CategoryId : ParentCategoryId;
    if (filterCategoryId > 0) {
        const char* categorySql =
            "SELECT COUNT(*) FROM account_categories WHERE id=? AND ledger_id=?;";
        sqlite3_stmt* categoryStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, categorySql, -1, &categoryStmt, nullptr) != SQLITE_OK) {
            OutError = "Failed to validate category filter";
            return false;
        }
        sqlite3_bind_int(categoryStmt, 1, filterCategoryId);
        sqlite3_bind_int(categoryStmt, 2, LedgerId);
        const bool categoryValid =
            sqlite3_step(categoryStmt) == SQLITE_ROW
            && sqlite3_column_int(categoryStmt, 0) == 1;
        sqlite3_finalize(categoryStmt);
        if (!categoryValid) {
            OutError = "Category filter does not belong to ledger";
            return false;
        }
    }

    if (Offset < 0) Offset = 0;
    if (Limit <= 0 || Limit > 200) Limit = 50;

    string whereSql = " WHERE t.ledger_id=?";
    if (!DateFrom.empty()) whereSql += " AND t.transaction_date>=?";
    if (!DateTo.empty()) whereSql += " AND t.transaction_date<=?";
    if (CategoryId > 0) {
        whereSql += " AND t.category_id=?";
    } else if (ParentCategoryId > 0) {
        whereSql +=
            " AND (t.category_id=? OR t.category_id IN ("
            "SELECT id FROM account_categories WHERE ledger_id=? AND parent_id=?))";
    }
    if (!Type.empty()) whereSql += " AND t.type=?";

    auto bindFilters = [&](sqlite3_stmt* statement) {
        int index = 1;
        sqlite3_bind_int(statement, index++, LedgerId);
        if (!DateFrom.empty()) sqlite3_bind_text(statement, index++, DateFrom.c_str(), -1, SQLITE_TRANSIENT);
        if (!DateTo.empty()) sqlite3_bind_text(statement, index++, DateTo.c_str(), -1, SQLITE_TRANSIENT);
        if (CategoryId > 0) {
            sqlite3_bind_int(statement, index++, CategoryId);
        } else if (ParentCategoryId > 0) {
            sqlite3_bind_int(statement, index++, ParentCategoryId);
            sqlite3_bind_int(statement, index++, LedgerId);
            sqlite3_bind_int(statement, index++, ParentCategoryId);
        }
        if (!Type.empty()) sqlite3_bind_text(statement, index++, Type.c_str(), -1, SQLITE_TRANSIENT);
        return index;
    };

    sqlite3_stmt* stmt = nullptr;
    const string countSql = "SELECT COUNT(*) FROM transactions t" + whereSql + ";";
    if (sqlite3_prepare_v2(m_Db, countSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare transaction count";
        return false;
    }
    bindFilters(stmt);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        OutError = "Failed to count transactions";
        return false;
    }
    OutTotal = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const bool ascending = SortOrder == "asc";
    const string orderColumn = SortBy == "amount" ? "t.amount" : "t.transaction_date";
    const string dataSql =
        "SELECT t.id, t.category_id, c.name, t.amount, t.type, t.description, "
        "t.transaction_date, u.username, c.parent_id, COALESCE(p.name, ''), "
        "t.is_voice_input, t.voice_text, t.voice_parse_status, t.voice_parse_error, "
        "t.voice_parsed_at, t.voice_parse_completed, t.created_by "
        "FROM transactions t "
        "JOIN account_categories c ON c.id=t.category_id AND c.ledger_id=t.ledger_id "
        "LEFT JOIN account_categories p ON p.id=c.parent_id AND p.ledger_id=c.ledger_id "
        "JOIN users u ON u.id=t.created_by" + whereSql
        + " ORDER BY " + orderColumn + (ascending ? " ASC" : " DESC")
        + ", t.id DESC LIMIT ? OFFSET ?;";
    if (sqlite3_prepare_v2(m_Db, dataSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare transaction query";
        return false;
    }
    int bindIndex = bindFilters(stmt);
    sqlite3_bind_int(stmt, bindIndex++, Limit);
    sqlite3_bind_int(stmt, bindIndex, Offset);

    const bool canManageAll = CanManageLedgerNoLock(m_Db, LedgerId, userId);
    ostringstream json;
    json << "[";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const bool canModify = canManageAll || sqlite3_column_int(stmt, 16) == userId;
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"id\":" << sqlite3_column_int(stmt, 0) << ","
             << "\"ledgerId\":" << LedgerId << ","
             << "\"categoryId\":" << sqlite3_column_int(stmt, 1) << ","
             << "\"categoryName\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 2), "")) << "\","
             << "\"amount\":" << sqlite3_column_double(stmt, 3) << ","
             << "\"type\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 4), "expense")) << "\","
             << "\"description\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 5), "")) << "\","
             << "\"date\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 6), "")) << "\","
             << "\"createdBy\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 7), "")) << "\","
             << "\"parentCategoryId\":" << sqlite3_column_int(stmt, 8) << ","
             << "\"parentCategoryName\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 9), "")) << "\","
             << "\"isVoiceInput\":" << sqlite3_column_int(stmt, 10) << ","
             << "\"voiceText\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 11), "")) << "\","
             << "\"voiceParseStatus\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 12), "")) << "\","
             << "\"voiceParseError\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 13), "")) << "\","
             << "\"voiceParsedAt\":\"" << EscapeJsonString(SafeCStr((const char*)sqlite3_column_text(stmt, 14), "")) << "\","
             << "\"voiceParseCompleted\":" << sqlite3_column_int(stmt, 15) << ","
             << "\"canEdit\":" << (canModify ? "true" : "false") << ","
             << "\"canDelete\":" << (canModify ? "true" : "false")
             << "}";
    }
    const int queryRc = sqlite3_errcode(m_Db);
    sqlite3_finalize(stmt);
    if (queryRc != SQLITE_OK && queryRc != SQLITE_DONE && queryRc != SQLITE_ROW) {
        OutJson = "[]";
        OutTotal = 0;
        OutError = "Failed to query transactions";
        return false;
    }

    json << "]";
    OutJson = json.str();
    return true;
}

bool LedgerManager::GetTransactionLedgerId(int TransactionId, int& OutLedgerId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutLedgerId = 0;
    if (!m_Db || TransactionId <= 0) return false;

    const char* sql = "SELECT ledger_id FROM transactions WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, TransactionId);
    if (sqlite3_step(stmt) == SQLITE_ROW) OutLedgerId = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return OutLedgerId > 0;
}

bool LedgerManager::UpdateLedgerTransaction(int TransactionId,
                                             int CategoryId,
                                             double Amount,
                                             const string& Type,
                                             const string& Description,
                                             const string& Date,
                                             const string& RequestedByUsername,
                                             string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();

    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (TransactionId <= 0 || CategoryId <= 0) { OutError = "Invalid transaction or category"; return false; }
    if (Amount <= 0.0) { OutError = "Transaction amount must be greater than zero"; return false; }
    if (Type != "expense" && Type != "income") { OutError = "Invalid transaction type"; return false; }
    if (Date.empty()) { OutError = "Transaction date is required"; return false; }

    const int userId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (userId <= 0) { OutError = "User not found"; return false; }

    int ledgerId = 0;
    int creatorId = 0;
    const char* loadSql = "SELECT ledger_id, created_by FROM transactions WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, loadSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to load transaction";
        return false;
    }
    sqlite3_bind_int(stmt, 1, TransactionId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ledgerId = sqlite3_column_int(stmt, 0);
        creatorId = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (ledgerId <= 0) { OutError = "Transaction not found"; return false; }

    const bool canModify =
        CanManageLedgerNoLock(m_Db, ledgerId, userId)
        || (CanAccessLedgerNoLock(m_Db, ledgerId, userId) && creatorId == userId);
    if (!canModify) { OutError = "Permission denied"; return false; }

    const char* categorySql =
        "SELECT COUNT(*) FROM account_categories WHERE id=? AND ledger_id=? AND type=?;";
    if (sqlite3_prepare_v2(m_Db, categorySql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to validate transaction category";
        return false;
    }
    sqlite3_bind_int(stmt, 1, CategoryId);
    sqlite3_bind_int(stmt, 2, ledgerId);
    sqlite3_bind_text(stmt, 3, Type.c_str(), -1, SQLITE_TRANSIENT);
    const bool categoryValid =
        sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    if (!categoryValid) {
        OutError = "Category does not belong to ledger or type mismatches";
        return false;
    }

    const char* updateSql =
        "UPDATE transactions SET category_id=?, amount=?, type=?, description=?, transaction_date=? "
        "WHERE id=? AND ledger_id=?;";
    if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare transaction update";
        return false;
    }
    sqlite3_bind_int(stmt, 1, CategoryId);
    sqlite3_bind_double(stmt, 2, Amount);
    sqlite3_bind_text(stmt, 3, Type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, Description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, Date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, TransactionId);
    sqlite3_bind_int(stmt, 7, ledgerId);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to update transaction";
        return false;
    }
    return true;
}

bool LedgerManager::DeleteLedgerTransaction(int TransactionId,
                                             const string& RequestedByUsername,
                                             string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutError.clear();

    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (TransactionId <= 0) { OutError = "Invalid transaction"; return false; }

    const int userId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (userId <= 0) { OutError = "User not found"; return false; }

    int ledgerId = 0;
    int creatorId = 0;
    const char* loadSql = "SELECT ledger_id, created_by FROM transactions WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, loadSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to load transaction";
        return false;
    }
    sqlite3_bind_int(stmt, 1, TransactionId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ledgerId = sqlite3_column_int(stmt, 0);
        creatorId = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (ledgerId <= 0) { OutError = "Transaction not found"; return false; }

    const bool canDelete =
        CanManageLedgerNoLock(m_Db, ledgerId, userId)
        || (CanAccessLedgerNoLock(m_Db, ledgerId, userId) && creatorId == userId);
    if (!canDelete) { OutError = "Permission denied"; return false; }

    const char* deleteSql = "DELETE FROM transactions WHERE id=? AND ledger_id=?;";
    if (sqlite3_prepare_v2(m_Db, deleteSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare transaction deletion";
        return false;
    }
    sqlite3_bind_int(stmt, 1, TransactionId);
    sqlite3_bind_int(stmt, 2, ledgerId);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(m_Db) <= 0) {
        OutError = "Failed to delete transaction";
        return false;
    }
    return true;
}

bool LedgerManager::UpsertImportedTransaction(int LedgerId,
                                               const FLedgerImportedTransaction& Entry,
                                               const string& CreatedBy,
                                               bool& bOutInserted,
                                               int& OutId,
                                               string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    bOutInserted = false;
    OutId = 0;
    OutError.clear();

    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0 || Entry.ImportSource.empty() || Entry.ImportSourceKey.empty()) {
        OutError = "Missing ledger or import source key";
        return false;
    }
    if (Entry.Amount <= 0.0 || Entry.Date.empty()) {
        OutError = "Invalid imported transaction amount or date";
        return false;
    }

    const int userId = GetUserIdNoLock(m_Db, CreatedBy);
    if (userId <= 0) { OutError = "User not found"; return false; }
    if (!CanAccessLedgerNoLock(m_Db, LedgerId, userId)) {
        OutError = "Permission denied";
        return false;
    }

    const string type = Entry.Type == "income" ? "income" : "expense";
    auto findCategoryId = [&](const string& categoryName) -> int {
        int categoryId = 0;
        const char* catSql =
            "SELECT id FROM account_categories WHERE ledger_id=? AND name=? AND type=? "
            "ORDER BY parent_id DESC, is_system DESC, id LIMIT 1;";
        sqlite3_stmt* catStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, catSql, -1, &catStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(catStmt, 1, LedgerId);
            sqlite3_bind_text(catStmt, 2, categoryName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(catStmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(catStmt) == SQLITE_ROW) categoryId = sqlite3_column_int(catStmt, 0);
        }
        sqlite3_finalize(catStmt);
        return categoryId;
    };

    int categoryId = findCategoryId(Entry.CategoryName);
    if (categoryId <= 0) categoryId = findCategoryId(type == "income" ? "其他收入" : "其他支出");
    if (categoryId <= 0) { OutError = "Fallback category not found"; return false; }

    int existingId = 0;
    const char* findSql =
        "SELECT id FROM transactions WHERE ledger_id=? AND import_source=? AND import_source_key=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, findSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare import lookup";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_text(stmt, 2, Entry.ImportSource.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, Entry.ImportSourceKey.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) existingId = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    string description = Entry.Description.empty() ? Entry.ImportRawText : Entry.Description;
    if (!Entry.ImportRawText.empty()) description += "\n原始流水：" + Entry.ImportRawText;

    if (existingId > 0) {
        const char* updateSql =
            "UPDATE transactions SET category_id=?, amount=?, type=?, description=?, transaction_date=?, "
            "created_by=?, is_voice_input=0, voice_text='', voice_parse_status='', voice_parse_error='', voice_parsed_at='', "
            "import_raw_text=?, imported_at=datetime('now','localtime') WHERE id=? AND ledger_id=?;";
        if (sqlite3_prepare_v2(m_Db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
            OutError = "Failed to prepare import update";
            return false;
        }
        sqlite3_bind_int(stmt, 1, categoryId);
        sqlite3_bind_double(stmt, 2, Entry.Amount);
        sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, Entry.Date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, userId);
        sqlite3_bind_text(stmt, 7, Entry.ImportRawText.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, existingId);
        sqlite3_bind_int(stmt, 9, LedgerId);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) { OutError = "Failed to update imported transaction"; return false; }
        OutId = existingId;
        return true;
    }

    const char* insertSql =
        "INSERT INTO transactions "
        "(ledger_id, category_id, amount, type, description, created_by, transaction_date, "
        " import_source, import_source_key, import_raw_text, imported_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now','localtime'));";
    if (sqlite3_prepare_v2(m_Db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare import insert";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_int(stmt, 2, categoryId);
    sqlite3_bind_double(stmt, 3, Entry.Amount);
    sqlite3_bind_text(stmt, 4, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, userId);
    sqlite3_bind_text(stmt, 7, Entry.Date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, Entry.ImportSource.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, Entry.ImportSourceKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, Entry.ImportRawText.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { OutError = "Failed to insert imported transaction"; return false; }
    OutId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
    bOutInserted = true;
    return true;
}

bool LedgerManager::CreateVoiceInputTransaction(int LedgerId, const string& VoiceText,
                                                 const string& CreatedBy,
                                                 int& OutId, string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    OutId = 0;
    OutError.clear();

    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (LedgerId <= 0 || VoiceText.empty()) { OutError = "Missing ledgerId or text"; return false; }

    const int userId = GetUserIdNoLock(m_Db, CreatedBy);
    if (userId <= 0) { OutError = "User not found"; return false; }
    if (!CanAccessLedgerNoLock(m_Db, LedgerId, userId)) {
        OutError = "Permission denied";
        return false;
    }

    int categoryId = 0;
    const char* findCatSql =
        "SELECT id FROM account_categories "
        "WHERE ledger_id=? AND type='expense' AND name='其他支出' "
        "ORDER BY parent_id, is_system DESC, id LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, findCatSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare fallback category lookup";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    if (sqlite3_step(stmt) == SQLITE_ROW) categoryId = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (categoryId <= 0) {
        OutError = "Fallback category not found";
        return false;
    }

    time_t now = time(nullptr);
    struct tm tmInfo;
#ifdef _WIN32
    localtime_s(&tmInfo, &now);
#else
    localtime_r(&now, &tmInfo);
#endif
    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tmInfo);

    const char* insertSql =
        "INSERT INTO transactions "
        "(ledger_id, category_id, amount, type, description, created_by, transaction_date, "
        " is_voice_input, voice_text, voice_parse_status, voice_parse_error, voice_parsed_at, voice_parse_completed) "
        "VALUES (?, ?, 0, 'expense', ?, ?, ?, 1, ?, 'pending', '', '', 0);";
    if (sqlite3_prepare_v2(m_Db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare voice transaction";
        return false;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_int(stmt, 2, categoryId);
    sqlite3_bind_text(stmt, 3, VoiceText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, userId);
    sqlite3_bind_text(stmt, 5, dateBuf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, VoiceText.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        OutError = "Failed to create voice transaction";
        return false;
    }
    OutId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
    return true;
}

bool LedgerManager::RequeueVoiceInputTransaction(int TransactionId,
                                                 const string& Username,
                                                 string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) { OutError = "Database unavailable"; return false; }

    int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) { OutError = "User not found"; return false; }

    const char* sql =
        "UPDATE transactions SET voice_parse_status='pending', voice_parse_error='', voice_parsed_at='', voice_parse_completed=0 "
        "WHERE id=? AND is_voice_input=1 AND voice_parse_status NOT IN ('pending','processing') AND created_by=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to prepare voice requeue";
        return false;
    }
    sqlite3_bind_int(stmt, 1, TransactionId);
    sqlite3_bind_int(stmt, 2, userId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(m_Db) <= 0) {
        OutError = "Voice transaction not found or cannot be requeued";
        return false;
    }
    return true;
}

bool LedgerManager::GetPendingVoiceInputTransactions(vector<FLedgerVoicePendingItem>& OutItems,
                                                     int Limit)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;
    if (Limit <= 0 || Limit > 50) Limit = 5;

    const char* sql =
        "SELECT t.id, t.ledger_id, t.voice_text, u.username "
        "FROM transactions t JOIN users u ON t.created_by=u.id "
        "WHERE t.is_voice_input=1 AND t.voice_parse_status='pending' "
        "ORDER BY t.id LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, Limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FLedgerVoicePendingItem item;
        item.Id = sqlite3_column_int(stmt, 0);
        item.LedgerId = sqlite3_column_int(stmt, 1);
        item.VoiceText = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "");
        item.Username = SafeCStr((const char*)sqlite3_column_text(stmt, 3), "");
        OutItems.push_back(item);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool LedgerManager::MarkVoiceInputProcessing(int TransactionId)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;
    const char* sql =
        "UPDATE transactions SET voice_parse_status='processing', voice_parse_error='', voice_parse_completed=0 "
        "WHERE id=? AND is_voice_input=1 AND voice_parse_status='pending';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, TransactionId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

bool LedgerManager::MarkVoiceInputFailed(int TransactionId, const string& Error)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;
    const char* sql =
        "UPDATE transactions SET voice_parse_status='failed', voice_parse_error=?, voice_parsed_at=datetime('now','localtime'), voice_parse_completed=0 "
        "WHERE id=? AND is_voice_input=1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, Error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, TransactionId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

bool LedgerManager::ApplyVoiceParseResult(int TransactionId,
                                          const vector<FLedgerParsedVoiceEntry>& Entries,
                                          string& OutError)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) { OutError = "Database unavailable"; return false; }
    if (Entries.empty()) { OutError = "No parsed entries"; return false; }

    int ledgerId = 0, userId = 0;
    string originalVoiceText;
    const char* loadSql =
        "SELECT ledger_id, created_by, voice_text FROM transactions "
        "WHERE id=? AND is_voice_input=1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, loadSql, -1, &stmt, nullptr) != SQLITE_OK) {
        OutError = "Failed to load voice transaction";
        return false;
    }
    sqlite3_bind_int(stmt, 1, TransactionId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ledgerId = sqlite3_column_int(stmt, 0);
        userId = sqlite3_column_int(stmt, 1);
        originalVoiceText = SafeCStr((const char*)sqlite3_column_text(stmt, 2), "");
    }
    sqlite3_finalize(stmt);
    if (ledgerId <= 0 || userId <= 0) { OutError = "Voice transaction not found"; return false; }

    auto findCategoryId = [&](const string& categoryName, const string& type) -> int {
        int categoryId = 0;
        const char* catSql =
            "SELECT id FROM account_categories WHERE ledger_id=? AND name=? AND type=? "
            "ORDER BY parent_id DESC, is_system DESC, id LIMIT 1;";
        sqlite3_stmt* catStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, catSql, -1, &catStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(catStmt, 1, ledgerId);
            sqlite3_bind_text(catStmt, 2, categoryName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(catStmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(catStmt) == SQLITE_ROW) categoryId = sqlite3_column_int(catStmt, 0);
        }
        sqlite3_finalize(catStmt);
        return categoryId;
    };

    auto fallbackCategoryId = [&](const string& type) -> int {
        return findCategoryId(type == "income" ? "其他收入" : "其他支出", type);
    };

    if (sqlite3_exec(m_Db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        OutError = "Failed to begin transaction";
        return false;
    }

    bool ok = true;
    string err;
    for (size_t i = 0; i < Entries.size() && ok; ++i) {
        FLedgerParsedVoiceEntry e = Entries[i];
        if (e.Type != "income") e.Type = "expense";
        if (e.Amount < 0) e.Amount = 0;
        if (e.Description.empty()) e.Description = originalVoiceText;
        int categoryId = findCategoryId(e.CategoryName, e.Type);
        if (categoryId <= 0) categoryId = fallbackCategoryId(e.Type);
        if (categoryId <= 0) { ok = false; err = "Fallback category not found"; break; }
        string desc = (i > 0 ? "[分段" + to_string(i + 1) + "]" : "") + e.Description;

        if (i == 0) {
            const char* updateSql =
                "UPDATE transactions SET category_id=?, amount=?, type=?, description=?, "
                "voice_parse_status='done', voice_parse_error='', voice_parsed_at=datetime('now','localtime'), voice_parse_completed=? "
                "WHERE id=?;";
            sqlite3_stmt* upStmt = nullptr;
            if (sqlite3_prepare_v2(m_Db, updateSql, -1, &upStmt, nullptr) != SQLITE_OK) { ok = false; err = "Prepare update failed"; break; }
            sqlite3_bind_int(upStmt, 1, categoryId);
            sqlite3_bind_double(upStmt, 2, e.Amount);
            sqlite3_bind_text(upStmt, 3, e.Type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upStmt, 4, desc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upStmt, 5, e.ParseCompleted ? 1 : 0);
            sqlite3_bind_int(upStmt, 6, TransactionId);
            int rc = sqlite3_step(upStmt);
            sqlite3_finalize(upStmt);
            if (rc != SQLITE_DONE) { ok = false; err = "Update voice transaction failed"; }
        } else {
            const char* insertSql =
                "INSERT INTO transactions "
                "(ledger_id, category_id, amount, type, description, created_by, transaction_date, "
                " is_voice_input, voice_text, voice_parse_status, voice_parse_error, voice_parsed_at, voice_parse_completed) "
                "SELECT ledger_id, ?, ?, ?, ?, created_by, transaction_date, "
                "1, voice_text, 'done', '', datetime('now','localtime'), ? "
                "FROM transactions WHERE id=? AND ledger_id=?;";
            sqlite3_stmt* insStmt = nullptr;
            if (sqlite3_prepare_v2(m_Db, insertSql, -1, &insStmt, nullptr) != SQLITE_OK) { ok = false; err = "Prepare segment insert failed"; break; }
            sqlite3_bind_int(insStmt, 1, categoryId);
            sqlite3_bind_double(insStmt, 2, e.Amount);
            sqlite3_bind_text(insStmt, 3, e.Type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insStmt, 4, desc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insStmt, 5, e.ParseCompleted ? 1 : 0);
            sqlite3_bind_int(insStmt, 6, TransactionId);
            sqlite3_bind_int(insStmt, 7, ledgerId);
            int rc = sqlite3_step(insStmt);
            sqlite3_finalize(insStmt);
            if (rc != SQLITE_DONE) { ok = false; err = "Insert segment transaction failed"; }
        }
    }

    if (!ok) {
        sqlite3_exec(m_Db, "ROLLBACK;", nullptr, nullptr, nullptr);
        OutError = err.empty() ? "Failed to apply parse result" : err;
        return false;
    }
    sqlite3_exec(m_Db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

// ============================================================================
// 统计
// ============================================================================

bool LedgerManager::GetLedgerStats(int LedgerId,
                                   const string& RequestedByUsername,
                                   const string& DateFrom,
                                   const string& DateTo,
                                   const string& GroupBy,
                                   string& OutJson,
                                   string& OutError,
                                   int ParentCategoryId)
{
    lock_guard<mutex> lock(m_Mutex);
    OutJson = "{}";
    OutError.clear();
    (void)GroupBy;

    if (!m_Db) {
        OutError = "Database is not open";
        return false;
    }
    if (LedgerId <= 0) {
        OutError = "Invalid ledger ID";
        return false;
    }
    if (ParentCategoryId < 0) {
        OutError = "Invalid parent category ID";
        return false;
    }

    const int userId = GetUserIdNoLock(m_Db, RequestedByUsername);
    if (userId <= 0 || !CanAccessLedgerNoLock(m_Db, LedgerId, userId)) {
        OutError = "No permission to access ledger";
        return false;
    }

    // 钻取子分类前先校验父分类确实是目标账本的一级分类，防止跨账本分类 ID 污染统计范围。
    if (ParentCategoryId > 0) {
        const char* parentSql =
            "SELECT COUNT(*) FROM account_categories "
            "WHERE id=? AND ledger_id=? AND parent_id=0;";
        sqlite3_stmt* parentStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, parentSql, -1, &parentStmt, nullptr) != SQLITE_OK) {
            OutError = sqlite3_errmsg(m_Db);
            return false;
        }
        sqlite3_bind_int(parentStmt, 1, ParentCategoryId);
        sqlite3_bind_int(parentStmt, 2, LedgerId);
        const bool parentExists =
            sqlite3_step(parentStmt) == SQLITE_ROW
            && sqlite3_column_int(parentStmt, 0) > 0;
        sqlite3_finalize(parentStmt);
        if (!parentExists) {
            OutError = "Parent category does not belong to ledger";
            return false;
        }
    }

    string filterSql = " WHERE t.ledger_id=?";
    if (!DateFrom.empty()) filterSql += " AND t.transaction_date>=?";
    if (!DateTo.empty()) filterSql += " AND t.transaction_date<=?";

    auto bindCommonFilters = [&](sqlite3_stmt* Statement, int StartIndex) -> int {
        int index = StartIndex;
        sqlite3_bind_int(Statement, index++, LedgerId);
        if (!DateFrom.empty()) {
            sqlite3_bind_text(Statement, index++, DateFrom.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (!DateTo.empty()) {
            sqlite3_bind_text(Statement, index++, DateTo.c_str(), -1, SQLITE_TRANSIENT);
        }
        return index;
    };

    double totalExpense = 0.0;
    double totalIncome = 0.0;
    const string totalSql =
        "SELECT t.type, COALESCE(SUM(t.amount), 0) FROM transactions t"
        + filterSql + " GROUP BY t.type;";
    sqlite3_stmt* totalStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, totalSql.c_str(), -1, &totalStmt, nullptr) != SQLITE_OK) {
        OutError = sqlite3_errmsg(m_Db);
        return false;
    }
    bindCommonFilters(totalStmt, 1);
    int stepResult = SQLITE_ROW;
    while ((stepResult = sqlite3_step(totalStmt)) == SQLITE_ROW) {
        const string type = SafeCStr((const char*)sqlite3_column_text(totalStmt, 0), "");
        const double amount = sqlite3_column_double(totalStmt, 1);
        if (type == "expense") totalExpense = amount;
        else if (type == "income") totalIncome = amount;
    }
    sqlite3_finalize(totalStmt);
    if (stepResult != SQLITE_DONE) {
        OutError = sqlite3_errmsg(m_Db);
        return false;
    }

    string categorySql =
        "SELECT c.name, t.type, COALESCE(SUM(t.amount), 0), COUNT(*), c.id, c.parent_id "
        "FROM transactions t "
        "JOIN account_categories c ON c.id=t.category_id AND c.ledger_id=t.ledger_id"
        + filterSql;
    if (ParentCategoryId > 0) {
        categorySql += " AND c.parent_id=?";
    } else {
        categorySql += " AND c.parent_id=0";
    }
    categorySql +=
        " GROUP BY c.id, c.name, c.parent_id, t.type"
        " ORDER BY SUM(t.amount) DESC, c.id ASC;";

    sqlite3_stmt* categoryStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, categorySql.c_str(), -1, &categoryStmt, nullptr) != SQLITE_OK) {
        OutError = sqlite3_errmsg(m_Db);
        return false;
    }
    int nextIndex = bindCommonFilters(categoryStmt, 1);
    if (ParentCategoryId > 0) {
        sqlite3_bind_int(categoryStmt, nextIndex, ParentCategoryId);
    }

    ostringstream categoryArray;
    categoryArray << "[";
    bool firstCategory = true;
    while ((stepResult = sqlite3_step(categoryStmt)) == SQLITE_ROW) {
        if (!firstCategory) categoryArray << ",";
        firstCategory = false;
        categoryArray << "{"
                      << "\"categoryId\":" << sqlite3_column_int(categoryStmt, 4) << ","
                      << "\"parentId\":" << sqlite3_column_int(categoryStmt, 5) << ","
                      << "\"name\":\"" << EscapeJsonString(
                             SafeCStr((const char*)sqlite3_column_text(categoryStmt, 0), "")) << "\","
                      << "\"type\":\"" << EscapeJsonString(
                             SafeCStr((const char*)sqlite3_column_text(categoryStmt, 1), "expense")) << "\","
                      << "\"amount\":" << sqlite3_column_double(categoryStmt, 2) << ","
                      << "\"count\":" << sqlite3_column_int(categoryStmt, 3)
                      << "}";
    }
    sqlite3_finalize(categoryStmt);
    if (stepResult != SQLITE_DONE) {
        OutError = sqlite3_errmsg(m_Db);
        return false;
    }
    categoryArray << "]";

    const string dailySql =
        "SELECT t.transaction_date, t.type, COALESCE(SUM(t.amount), 0) "
        "FROM transactions t" + filterSql
        + " GROUP BY t.transaction_date, t.type ORDER BY t.transaction_date;";
    sqlite3_stmt* dailyStmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, dailySql.c_str(), -1, &dailyStmt, nullptr) != SQLITE_OK) {
        OutError = sqlite3_errmsg(m_Db);
        return false;
    }
    bindCommonFilters(dailyStmt, 1);

    map<string, pair<double, double>> dailyMap;
    while ((stepResult = sqlite3_step(dailyStmt)) == SQLITE_ROW) {
        const string date = SafeCStr((const char*)sqlite3_column_text(dailyStmt, 0), "");
        const string type = SafeCStr((const char*)sqlite3_column_text(dailyStmt, 1), "");
        const double amount = sqlite3_column_double(dailyStmt, 2);
        if (type == "expense") dailyMap[date].first = amount;
        else if (type == "income") dailyMap[date].second = amount;
    }
    sqlite3_finalize(dailyStmt);
    if (stepResult != SQLITE_DONE) {
        OutError = sqlite3_errmsg(m_Db);
        return false;
    }

    ostringstream dailyArray;
    dailyArray << "[";
    bool firstDay = true;
    for (const auto& item : dailyMap) {
        if (!firstDay) dailyArray << ",";
        firstDay = false;
        dailyArray << "{"
                   << "\"date\":\"" << EscapeJsonString(item.first) << "\","
                   << "\"expense\":" << item.second.first << ","
                   << "\"income\":" << item.second.second
                   << "}";
    }
    dailyArray << "]";

    ostringstream response;
    response << "{"
             << "\"ledgerId\":" << LedgerId << ","
             << "\"totalExpense\":" << totalExpense << ","
             << "\"totalIncome\":" << totalIncome << ","
             << "\"balance\":" << (totalIncome - totalExpense) << ","
             << "\"categories\":" << categoryArray.str() << ","
             << "\"daily\":" << dailyArray.str()
             << "}";
    OutJson = response.str();
    return true;
}

// ============================================================================
// 工具方法
// ============================================================================

bool LedgerManager::IsGroupMember(int GroupId, const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || GroupId <= 0 || Username.empty()) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId < 0) return false;

    // Web 兼容命令仍使用 groupId 命名，但当前值实际是账本 ID。
    // 授权关系必须由“账本所属家庭 + 家庭成员”推导，禁止依赖已废弃的 group_members。
    const char* sql =
        "SELECT 1 "
        "FROM ledgers l "
        "JOIN family_members fm ON fm.family_id=l.family_id "
        "WHERE l.id=? AND fm.user_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, GroupId);
    sqlite3_bind_int(stmt, 2, userId);

    const bool bIsMember = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return bIsMember;
}

bool LedgerManager::IsGroupAdmin(int GroupId, const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db || GroupId <= 0 || Username.empty()) return false;

    const int userId = GetUserIdNoLock(m_Db, Username);
    return userId > 0 && CanManageLedgerNoLock(m_Db, GroupId, userId);
}

int LedgerManager::GetUserId(const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    return GetUserIdNoLock(m_Db, Username);
}

// ============================================================================
// 账单导入任务队列
// ============================================================================

int LedgerManager::CreateBillImportTask(int LedgerId, const string& Username,
                                        const string& Filename,
                                        const string& FileContent)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return 0;
    if (LedgerId <= 0 || Username.empty() || Filename.empty()) return 0;

    const int userId = GetUserIdNoLock(m_Db, Username);
    if (userId <= 0 || !CanAccessLedgerNoLock(m_Db, LedgerId, userId)) return 0;

    const char* sql = "INSERT INTO bill_import_tasks "
        "(ledger_id, username, filename, file_content, status) "
        "VALUES (?, ?, ?, ?, 'pending');";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_bind_int(stmt, 1, LedgerId);
    sqlite3_bind_text(stmt, 2, Username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, Filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 4, FileContent.data(), (int)FileContent.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;

    return (int)sqlite3_last_insert_rowid(m_Db);
}

bool LedgerManager::GetPendingBillImportTasks(vector<FBillImportTaskItem>& OutItems, int Limit)
{
    lock_guard<mutex> lock(m_Mutex);
    OutItems.clear();
    if (!m_Db) return false;
    if (Limit <= 0) Limit = 1;

    const char* sql = "SELECT id, ledger_id, username, filename, file_content, status, "
        "total_rows, imported_rows, inserted_rows, updated_rows, skipped_rows, "
        "source_type, message, error_message "
        "FROM bill_import_tasks WHERE status='pending' ORDER BY id LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_int(stmt, 1, Limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FBillImportTaskItem item;
        item.Id = sqlite3_column_int(stmt, 0);
        item.LedgerId = sqlite3_column_int(stmt, 1);
        if (sqlite3_column_text(stmt, 2)) item.Username = (const char*)sqlite3_column_text(stmt, 2);
        if (sqlite3_column_text(stmt, 3)) item.Filename = (const char*)sqlite3_column_text(stmt, 3);
        if (sqlite3_column_bytes(stmt, 4) > 0) {
            const void* blob = sqlite3_column_blob(stmt, 4);
            int blobSize = sqlite3_column_bytes(stmt, 4);
            item.FileContent.assign((const char*)blob, (size_t)blobSize);
        }
        if (sqlite3_column_text(stmt, 5)) item.Status = (const char*)sqlite3_column_text(stmt, 5);
        item.TotalRows = sqlite3_column_int(stmt, 6);
        item.ImportedRows = sqlite3_column_int(stmt, 7);
        item.InsertedRows = sqlite3_column_int(stmt, 8);
        item.UpdatedRows = sqlite3_column_int(stmt, 9);
        item.SkippedRows = sqlite3_column_int(stmt, 10);
        if (sqlite3_column_text(stmt, 11)) item.SourceType = (const char*)sqlite3_column_text(stmt, 11);
        if (sqlite3_column_text(stmt, 12)) item.Message = (const char*)sqlite3_column_text(stmt, 12);
        if (sqlite3_column_text(stmt, 13)) item.ErrorMessage = (const char*)sqlite3_column_text(stmt, 13);
        OutItems.push_back(item);
    }
    sqlite3_finalize(stmt);
    return !OutItems.empty();
}

bool LedgerManager::MarkBillImportProcessing(int TaskId)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql = "UPDATE bill_import_tasks SET status='processing', updated_at=datetime('now','localtime') WHERE id=? AND status='pending';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_int(stmt, 1, TaskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

bool LedgerManager::MarkBillImportDone(int TaskId, int TotalRows, int ImportedRows,
                                       int InsertedRows, int UpdatedRows, int SkippedRows,
                                       const string& SourceType,
                                       const string& Message)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql = "UPDATE bill_import_tasks SET status='done', "
        "total_rows=?, imported_rows=?, inserted_rows=?, updated_rows=?, skipped_rows=?, "
        "source_type=?, message=?, error_message='', updated_at=datetime('now','localtime') "
        "WHERE id=? AND status='processing';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_int(stmt, 1, TotalRows);
    sqlite3_bind_int(stmt, 2, ImportedRows);
    sqlite3_bind_int(stmt, 3, InsertedRows);
    sqlite3_bind_int(stmt, 4, UpdatedRows);
    sqlite3_bind_int(stmt, 5, SkippedRows);
    sqlite3_bind_text(stmt, 6, SourceType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, Message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, TaskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

bool LedgerManager::MarkBillImportFailed(int TaskId, const string& Error)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    const char* sql = "UPDATE bill_import_tasks SET status='failed', error_message=?, "
        "updated_at=datetime('now','localtime') WHERE id=? AND status IN ('pending','processing');";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_text(stmt, 1, Error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, TaskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(m_Db) > 0;
}

bool LedgerManager::GetBillImportTaskStatus(int TaskId, FBillImportTaskItem& OutItem)
{
    lock_guard<mutex> lock(m_Mutex);
    OutItem = FBillImportTaskItem{};
    if (!m_Db) return false;

    const char* sql = "SELECT id, ledger_id, username, filename, status, "
        "total_rows, imported_rows, inserted_rows, updated_rows, skipped_rows, "
        "source_type, message, error_message "
        "FROM bill_import_tasks WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_int(stmt, 1, TaskId);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        OutItem.Id = sqlite3_column_int(stmt, 0);
        OutItem.LedgerId = sqlite3_column_int(stmt, 1);
        if (sqlite3_column_text(stmt, 2)) OutItem.Username = (const char*)sqlite3_column_text(stmt, 2);
        if (sqlite3_column_text(stmt, 3)) OutItem.Filename = (const char*)sqlite3_column_text(stmt, 3);
        if (sqlite3_column_text(stmt, 4)) OutItem.Status = (const char*)sqlite3_column_text(stmt, 4);
        OutItem.TotalRows = sqlite3_column_int(stmt, 5);
        OutItem.ImportedRows = sqlite3_column_int(stmt, 6);
        OutItem.InsertedRows = sqlite3_column_int(stmt, 7);
        OutItem.UpdatedRows = sqlite3_column_int(stmt, 8);
        OutItem.SkippedRows = sqlite3_column_int(stmt, 9);
        if (sqlite3_column_text(stmt, 10)) OutItem.SourceType = (const char*)sqlite3_column_text(stmt, 10);
        if (sqlite3_column_text(stmt, 11)) OutItem.Message = (const char*)sqlite3_column_text(stmt, 11);
        if (sqlite3_column_text(stmt, 12)) OutItem.ErrorMessage = (const char*)sqlite3_column_text(stmt, 12);
    }
    sqlite3_finalize(stmt);
    return found;
}
bool LedgerManager::IsUserAdmin(const string& Username)
{
    lock_guard<mutex> lock(m_Mutex);
    if (!m_Db) return false;

    string sql = "SELECT permissions FROM users WHERE username=? AND is_active=1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, Username.c_str(), -1, SQLITE_TRANSIENT);

    bool isAdmin = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* perms = (const char*)sqlite3_column_text(stmt, 0);
        if (perms) {
            isAdmin = PermissionArrayContains(NormalizePermissionsJson(perms), "admin");
        }
    }
    sqlite3_finalize(stmt);
    return isAdmin;
}
