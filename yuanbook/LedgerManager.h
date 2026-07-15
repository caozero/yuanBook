// LedgerManager.h — 用户、家庭与账本领域数据库管理模块
//
// 使用 SQLite3 作为本地数据存储，核心领域边界如下：
//   - users：独立用户账号与当前 UI 上下文；上下文字段不参与授权判定。
//   - families / family_members：家庭及 owner/member 成员关系，是唯一授权来源。
//   - ledgers：归属于家庭的账本；家庭成员天然可访问家庭下全部账本。
//   - account_categories / transactions：严格归属于账本的数据。
//
// API: 查询接口主要返回 JSON 字符串，供 WebServer 直接通过 HTTP 响应；所有写接口
// 必须在领域层再次执行授权校验，禁止依赖前端隐藏入口保障安全。

#pragma once

#include <string>
#include <mutex>
#include <vector>
#include <cstdint>

// 前向声明 SQLite3 类型
struct sqlite3;

/** @brief 待处理语音流水的持久化任务快照。 */
struct FLedgerVoicePendingItem
{
    /** 语音流水 ID。 */
    int         Id = 0;
    /** 目标账本 ID，用于限定分类查询、解析结果回填和分段流水落库范围。 */
    int         LedgerId = 0;
    /** 原始语音文本。 */
    std::string VoiceText;
    /** 流水创建者用户名，用于账本访问授权和审计。 */
    std::string Username;
};

struct FLedgerParsedVoiceEntry
{
    std::string CategoryName;
    double      Amount = 0.0;
    std::string Type;
    std::string Description;
    bool        ParseCompleted = false;
};

struct FLedgerImportedTransaction
{
    std::string ImportSource;
    std::string ImportSourceKey;
    std::string ImportRawText;
    std::string CategoryName;
    double      Amount = 0.0;
    std::string Type;
    std::string Description;
    std::string Date;
};

struct FLedgerAuthSession
{
    std::string Token;
    std::string Username;
    int64_t     CreatedAt = 0;
};

/**
 * @brief 注册邀请的服务端可信快照。
 *
 * 注册客户端只能提交 Code；家庭、默认账本和家庭角色必须从该服务端记录解析，禁止信任
 * URL 或客户端请求中可被篡改的作用域参数。
 */
struct FLedgerInviteRegistration
{
    /** 邀请码，数据库主键。 */
    std::string Code;
    /** 注册成功后是否自动加入家庭。 */
    bool        bAutoJoinFamily = true;
    /** 自动加入的家庭 ID；不自动加入家庭时必须为 0。 */
    int         FamilyId = 0;
    /** 注册后的默认账本 ID；0 表示不指定默认账本。 */
    int         DefaultLedgerId = 0;
    /** 自动加入家庭后的角色，只允许 owner/member。 */
    std::string FamilyRole = "member";
    /** 邀请创建者用户 ID。 */
    int         CreatedByUserId = 0;
    /** 创建者用户名，仅用于展示与审计；授权始终以 CreatedByUserId 和数据库记录为准。 */
    std::string CreatedBy;
    /** 创建时间戳（Unix 秒）。 */
    int64_t     CreatedAt = 0;
    /** 过期时间戳（Unix 秒）。 */
    int64_t     ExpiresAt = 0;
};

/**
 * @brief 待审批动作基础记录。
 *
 * 该结构是所有“需要对方确认”的异步业务动作的统一抽象，当前首个落地场景为
 * 账本成员邀请。业务侧不得绕过该状态机直接写入最终业务表，否则会破坏审批一致性。
 */
struct FPendingActionBase
{
    /** 动作唯一 ID，数据库自增主键；0 表示尚未持久化。 */
    int         Id = 0;
    /** 动作类型，例如 group_member_invite。 */
    std::string ActionType;
    /** 状态：pending / accepted / rejected / expired / cancelled。 */
    std::string Status;
    /** 发起人用户 ID；0 表示旧数据或系统动作。 */
    int         CreatedByUserId = 0;
    /** 发起人用户名，仅用于展示与审计。 */
    std::string CreatedByUsername;
    /** 目标用户 ID。 */
    int         TargetUserId = 0;
    /** 目标用户名，仅用于展示与审计。 */
    std::string TargetUsername;
    /** 创建时间戳（Unix 秒）。 */
    int64_t     CreatedAt = 0;
    /** 过期时间戳（Unix 秒）；0 表示永不过期。 */
    int64_t     ExpiresAt = 0;
    /** 响应时间戳（Unix 秒）；0 表示尚未响应。 */
    int64_t     RespondedAt = 0;
    /** 业务负载 JSON。基础层不解释，仅由具体业务包装层解析。 */
    std::string PayloadJson;
};

/**
 * @brief 已有用户加入家庭的待确认邀请。
 *
 * 通过组合 FPendingActionBase 复用待审批动作状态机，同时保留强类型业务字段，避免 Web 层
 * 或前端直接解析 payload_json 形成脆弱耦合。
 */
struct FLedgerMemberInvite
{
    /** 基础待审批动作元数据，ActionType 固定为 family_member_invite。 */
    FPendingActionBase Base;
    /** 被邀请加入的家庭 ID。 */
    int         FamilyId = 0;
    /** 接受邀请后的家庭角色，只允许 owner/member。 */
    std::string Role = "member";
    /** 家庭名称，仅用于前端展示。 */
    std::string FamilyName;
    /**
     * 旧成员邀请实现兼容字段：调用方迁移完成前仅用于维持编译。
     * 新代码禁止读取，必须使用 FamilyId / FamilyName。
     */
    int         GroupId = 0;
    std::string GroupName;
};

struct FSystemSettingItem
{
    std::string SettingKey;
    std::string SettingValue;
    std::string ValueType;
    std::string Category;
    bool        bSensitive = false;
    std::string Description;
    std::string UpdatedBy;
    std::string UpdatedAt;
};

struct FSystemSettingChangedEvent
{
    std::string SettingKey;
    std::string OldValue;
    std::string NewValue;
    std::string ValueType;
    std::string Category;
    bool        bSensitive = false;
    std::string UpdatedBy;
    int64_t     UpdatedAtUnix = 0;
};

// ============================================================================
// 账单导入任务
// ============================================================================
/** @brief 账本导入任务的持久化快照。 */
struct FBillImportTaskItem
{
    /** 导入任务 ID。 */
    int         Id = 0;
    /** 导入目标账本 ID；分类和导入流水均不得越过该边界。 */
    int         LedgerId = 0;
    /** 发起导入的用户名，用于账本访问授权和流水创建者审计。 */
    std::string Username;
    std::string Filename;
    std::string FileContent;   // 文件二进制内容（仅 pending/processing 时有效）
    std::string Status;        // pending / processing / done / failed
    int         TotalRows = 0;
    int         ImportedRows = 0;
    int         InsertedRows = 0;
    int         UpdatedRows = 0;
    int         SkippedRows = 0;
    std::string SourceType;
    std::string Message;
    std::string ErrorMessage;
};

/** @brief PID 外部通道绑定的完整领域快照。 */
struct FLedgerPidBinding
{
    /** 绑定记录稳定 ID；更新与删除必须使用该 ID，禁止使用可见 PID 充当内部主键。 */
    int Id = 0;
    /** 全局唯一的外部接口凭证。 */
    std::string Pid;
    /** 当前定向写入的账本 ID。 */
    int LedgerId = 0;
    /** 目标账本名称，仅用于展示。 */
    std::string LedgerName;
    /** 目标账本所属家庭名称，仅用于展示。 */
    std::string FamilyName;
    /** PID 创建者用户 ID；外部流水以该用户作为创建者。 */
    int CreatorUserId = 0;
    /** PID 创建者用户名。 */
    std::string CreatorUsername;
    /** 创建者当前是否仍可访问目标账本。 */
    bool bAccessible = false;
    /** 创建时间。 */
    std::string CreatedAt;
    /** 最近改绑时间。 */
    std::string UpdatedAt;
};

/** @brief 外部 PID 请求解析后的定向写入上下文。 */
struct FLedgerPidRoute
{
    /** PID 绑定记录 ID。 */
    int BindingId = 0;
    /** 精确目标账本 ID。 */
    int LedgerId = 0;
    /** 创建者用户 ID，用于工作日志上下文。 */
    int CreatorUserId = 0;
    /** 创建者用户名，用于流水归属。 */
    std::string CreatorUsername;
};

class LedgerManager
{
public:
    LedgerManager();
    ~LedgerManager();

    // ---- 生命周期 ----
    // 打开/创建数据库文件，自动建表
    // DbPath: 数据库文件路径 (如 "./ledger.db")
    bool Initialize(const std::string& DbPath);
    void Shutdown();

    // ---- 系统参数管理 ----
    bool EnsureDefaultSystemSettings();
    bool GetSystemSettings(std::string& OutJson);
    bool GetSystemSettingValue(const std::string& SettingKey,
                               std::string& OutValue);
    bool UpsertSystemSetting(const FSystemSettingItem& Setting,
                             const std::string& UpdatedBy,
                             FSystemSettingChangedEvent* OutEvent = nullptr);
    bool DeleteSystemSetting(const std::string& SettingKey,
                             std::string& OutOldValue);

    // ---- 用户管理 ----

    /**
     * @brief 用户账号变更操作的统一执行结果。
     * @note 该结构不包含密码摘要等敏感数据，可直接供应用层序列化为管理接口响应。
     */
    struct FUserAccountMutationResult
    {
        /** 操作是否成功提交。 */
        bool bOk = false;
        /** 稳定错误码；成功时为空。 */
        std::string ErrorCode;
        /** 面向调用方的错误说明；成功时为空。 */
        std::string Message;
        /** 本次操作删除的持久化登录会话数量。 */
        int DeletedSessionCount = 0;
        /** 操作完成后的启用状态。 */
        bool bIsActive = false;
        /** 当前用户是否满足物理删除条件。 */
        bool bCanHardDelete = false;
    };

    // 认证用户：校验密码并返回权限
    bool AuthenticateUser(const std::string& Username,
                          const std::string& PasswordHash,
                          std::string& OutPermissions);
    // 读取指定用户当前保存的密码哈希（仅返回哈希本身，不做认证）
    bool GetUserPasswordHash(const std::string& Username,
                             std::string& OutPasswordHash);
    // 获取用户信息
    bool GetUserByUsername(const std::string& Username, std::string& OutJson);

    /**
     * @brief 列出指定用户创建的全部 PID 绑定。
     * @param Username 当前认证用户名。
     * @param OutJson 输出 JSON 数组；无记录时为 []。
     * @return 查询成功返回 true，用户不存在或数据库不可用返回 false。
     */
    bool GetLedgerPidBindings(const std::string& Username, std::string& OutJson);

    /**
     * @brief 创建一个全局唯一且必须绑定账本的 PID。
     * @param Username 创建者用户名，必须是目标账本所属家庭的有效成员。
     * @param Pid 外部凭证，去除首尾空白后长度必须为 1 至 80。
     * @param LedgerId 目标账本 ID。
     * @param OutJson 输出新建绑定对象。
     * @param OutError 输出稳定错误信息。
     * @return 创建成功返回 true。
     * @sideeffect 向 ledger_pid_bindings 插入一条持久化记录。
     */
    bool CreateLedgerPidBinding(const std::string& Username,
                                const std::string& Pid,
                                int LedgerId,
                                std::string& OutJson,
                                std::string& OutError);

    /**
     * @brief 修改当前用户所拥有 PID 的目标账本。
     * @param Username 当前认证用户名，仅允许修改自己创建的绑定。
     * @param BindingId 绑定记录 ID。
     * @param LedgerId 新目标账本 ID，必须由当前用户真实可访问。
     * @param OutJson 输出修改后的绑定对象。
     * @param OutError 输出稳定错误信息。
     * @return 改绑成功返回 true；失败时原绑定保持不变。
     */
    bool UpdateLedgerPidBindingLedger(const std::string& Username,
                                      int BindingId,
                                      int LedgerId,
                                      std::string& OutJson,
                                      std::string& OutError);

    /**
     * @brief 删除当前用户创建的 PID 绑定。
     * @param Username 当前认证用户名。
     * @param BindingId 绑定记录 ID。
     * @param OutError 输出稳定错误信息。
     * @return 精确删除一条记录返回 true；记录不存在或越权返回 false。
     */
    bool DeleteLedgerPidBinding(const std::string& Username,
                                int BindingId,
                                std::string& OutError);

    /**
     * @brief 将外部 PID 解析为精确账本和创建者上下文。
     * @param Pid 外部请求携带的 PID。
     * @param OutRoute 输出不可变路由结果，函数进入后重置。
     * @param OutError 输出失败原因。
     * @return PID 存在、创建者启用且仍可访问目标账本时返回 true。
     * @note 本接口绝不读取或修复用户当前账本，也不回退选择其他账本。
     */
    bool ResolveLedgerPidRoute(const std::string& Pid,
                               FLedgerPidRoute& OutRoute,
                               std::string& OutError);

    bool GetAllUsers(std::string& OutJson);
    bool CreateUser(const std::string& Username,
                    const std::string& PasswordHash,
                    const std::string& Permissions);
    bool UpdateUserPassword(const std::string& Username,
                            const std::string& NewPasswordHash);

    /**
     * @brief 原子更新指定用户密码摘要，并注销该用户的全部持久化登录会话。
     * @param Username 目标用户名，必须对应已存在且处于激活状态的用户。
     * @param NewPasswordHash 新密码摘要；空字符串表示清空密码并等待用户下次登录时初始化。
     * @param OutDeletedSessionCount 输出被删除的会话数量；失败时重置为 0。
     * @return 密码更新和会话删除均成功提交时返回 true；用户不存在或数据库操作失败返回 false。
     * @note 该函数在单一数据库事务内执行；成功后该用户所有现有 Token 均失效。
     */
    bool UpdateUserPasswordAndInvalidateSessions(const std::string& Username,
                                                 const std::string& NewPasswordHash,
                                                 int& OutDeletedSessionCount);

    /**
     * @brief 管理员设置或清空目标用户密码，并原子注销目标用户全部会话。
     * @param ActorUsername 发起操作的管理员用户名，用于执行自操作保护和管理员有效性检查。
     * @param TargetUsername 目标用户名，必须是启用账号且不能与 ActorUsername 相同。
     * @param NewPasswordHash 新密码摘要；空字符串表示清空密码并进入下次登录初始化状态。
     * @param OutResult 输出统一操作结果；函数进入后始终重置。
     * @return 操作成功提交时返回 true。
     * @sideeffect 更新 users.password_hash，并删除 auth_sessions 中目标用户的全部记录。
     */
    bool AdminUpdateUserPassword(const std::string& ActorUsername,
                                 const std::string& TargetUsername,
                                 const std::string& NewPasswordHash,
                                 FUserAccountMutationResult& OutResult);

    /**
     * @brief 启用或禁用目标用户；禁用时原子注销其全部会话。
     * @param ActorUsername 发起操作的管理员用户名，禁止操作自己。
     * @param TargetUsername 目标用户名。
     * @param bActive true 表示启用，false 表示禁用。
     * @param OutResult 输出统一操作结果。
     * @return 状态成功提交时返回 true；违反最后一个启用管理员保护等规则时返回 false。
     * @sideeffect 可能更新 users.is_active，并删除目标用户全部持久化会话。
     */
    bool SetUserActiveState(const std::string& ActorUsername,
                            const std::string& TargetUsername,
                            bool bActive,
                            FUserAccountMutationResult& OutResult);

    /**
     * @brief 删除用户账号，默认执行保留历史归属的软删除。
     * @param ActorUsername 发起操作的管理员用户名，禁止操作自己。
     * @param TargetUsername 目标用户名。
     * @param bHardDelete true 时仅在不存在任何业务引用的情况下物理删除；false 时执行软删除。
     * @param OutResult 输出统一操作结果及物理删除能力。
     * @return 删除语义成功提交时返回 true。
     * @sideeffect 软删除会设置 is_active=0；物理删除会移除 users 记录；两者均注销全部会话。
     */
    bool DeleteUserAccount(const std::string& ActorUsername,
                           const std::string& TargetUsername,
                           bool bHardDelete,
                           FUserAccountMutationResult& OutResult);

    bool UpdateUserPermissions(const std::string& Username,
                               const std::string& Permissions);
    bool DeleteUser(const std::string& Username);

    // ---- 邀请注册 ----
    /**
     * @brief 创建或更新注册邀请。
     * @param Code 四位邀请码，非空且由调用方完成大写标准化。
     * @param bAutoJoinFamily 注册成功后是否自动加入家庭。
     * @param FamilyId 目标家庭 ID；独立邀请必须为 0。
     * @param DefaultLedgerId 注册后的 UI 默认账本；必须属于 FamilyId，独立邀请必须为 0。
     * @param FamilyRole 加入家庭后的角色，只允许 owner/member。
     * @param CreatedBy 创建者用户名，必须是系统 admin 或目标家庭 owner。
     * @param bAllowNonAdminIndependentInvite 是否允许家庭 owner 创建独立邀请；系统 admin 不受该值限制。
     * @param ExpiresAt 过期 Unix 秒，必须晚于当前时间。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权、作用域约束和持久化全部成功时返回 true。
     */
    bool SaveInviteRegistration(const std::string& Code,
                                bool bAutoJoinFamily,
                                int FamilyId,
                                int DefaultLedgerId,
                                const std::string& FamilyRole,
                                const std::string& CreatedBy,
                                bool bAllowNonAdminIndependentInvite,
                                int64_t ExpiresAt,
                                std::string& OutError);
    bool GetValidInviteRegistration(const std::string& Code,
                                    FLedgerInviteRegistration& OutInvite,
                                    std::string& OutError);
    /**
     * @brief 判断用户是否至少拥有一个 owner 身份的家庭。
     * @param Username 待检查用户名。
     * @return 用户存在且在任意家庭中担任 owner 时返回 true；数据库不可用、用户不存在或仅为 member 时返回 false。
     * @note 该查询只提供领域能力事实，不读取运行时参数，也不授予任何具体家庭之外的访问权限。
     */
    bool HasOwnedFamily(const std::string& Username);
    /**
     * @brief 查询创建者在指定注册作用域下尚未过期的最近邀请。
     * @param CreatedBy 创建者用户名。
     * @param bAutoJoinFamily 是否自动加入家庭。
     * @param FamilyId 目标家庭 ID；迁移期为 0 且 DefaultLedgerId 有效时由服务端按账本归属推导。
     * @param DefaultLedgerId 注册后的默认账本 ID。
     * @param FamilyRole 注册后家庭角色，只允许 owner/member。
     * @param OutInvite 输出匹配的服务端邀请快照。
     * @param OutError 输出查询错误；没有匹配邀请属于正常结果，保持为空。
     * @return 找到匹配邀请时返回 true。
     */
    bool GetCurrentInviteRegistration(const std::string& CreatedBy,
                                      bool bAutoJoinFamily,
                                      int FamilyId,
                                      int DefaultLedgerId,
                                      const std::string& FamilyRole,
                                      FLedgerInviteRegistration& OutInvite,
                                      std::string& OutError);
    int  CleanupExpiredInviteRegistrations(int64_t Now);
    bool ApplyInviteRegistrationToUser(const FLedgerInviteRegistration& Invite,
                                       const std::string& Username,
                                       std::string& OutError);
    bool ApplyInviteCodeToExistingUser(const std::string& Code,
                                       const std::string& Username,
                                       FLedgerInviteRegistration& OutInvite,
                                       std::string& OutError);
    /**
     * @brief 使用服务端邀请记录原子注册新用户。
     * @param Code 邀请码；家庭、默认账本和角色均只从服务端记录读取。
     * @param Username 新用户名。
     * @param PasswordHash 新用户密码哈希。
     * @param Permissions 新用户系统权限 JSON，通常为 ["user"]。
     * @param OutInvite 输出实际消费的服务端邀请快照。
     * @param OutError 输出失败原因；任一步失败时用户和家庭关系均回滚。
     * @return 用户创建、家庭加入和 UI 上下文设置在同一事务内提交成功时返回 true。
     */
    bool RegisterUserWithInvite(const std::string& Code,
                                const std::string& Username,
                                const std::string& PasswordHash,
                                const std::string& Permissions,
                                FLedgerInviteRegistration& OutInvite,
                                std::string& OutError);

    // ---- 待审批成员邀请 ----
    /**
     * @brief 创建一个待对方确认的账本成员邀请。
     * @param GroupId 目标账本/家庭组 ID，必须大于 0。
     * @param InviteeUsername 被邀请用户名，必须是已存在的激活用户。
     * @param Role 接受后写入 family_members 的角色；兼容请求仅允许 member，其他值归一化为 member。
     * @param InvitedByUsername 邀请发起人用户名，必须是账本所属家庭 owner。
     * @param ExpiresInSec 邀请有效期秒数，<=0 时由调用方传入默认值后再进入此函数。
     * @param OutInvite 输出创建或复用的待审批邀请记录。
     * @param OutError 失败原因；成功时清空。
     * @return 创建或复用 pending 邀请成功返回 true。
     * @note 该函数只创建 pending 动作，不会直接写入 family_members。
     */
    bool CreateMemberInvite(int GroupId,
                            const std::string& InviteeUsername,
                            const std::string& Role,
                            const std::string& InvitedByUsername,
                            int ExpiresInSec,
                            FLedgerMemberInvite& OutInvite,
                            std::string& OutError);

    /**
     * @brief 查询指定用户收到的待处理家庭成员邀请。
     * @param Username 当前登录用户名；必须对应已存在用户。
     * @param OutJson 成功时输出 JSON 数组；失败时重置为空数组。
     * @param OutError 失败原因；成功时清空。数据库底层错误仅写入服务端日志，避免泄漏内部结构。
     * @return 用户解析、SQL 准备及结果遍历全部成功时返回 true。
     * @note 查询只读取 `family_member_invite`，并通过 `pending_actions.family_id` 关联家庭。
     */
    bool GetIncomingMemberInvites(const std::string& Username,
                                  std::string& OutJson,
                                  std::string& OutError);

    /**
     * @brief 查询指定管理员在某账本发出的成员邀请。
     * @param GroupId 账本/家庭组 ID。
     * @param Username 当前登录用户名，必须是该账本 owner/admin。
     * @param OutJson 输出 JSON 数组。
     * @param OutError 失败原因。
     * @return 查询成功返回 true。
     */
    bool GetSentMemberInvites(int GroupId,
                              const std::string& Username,
                              std::string& OutJson,
                              std::string& OutError);

    /**
     * @brief 接受成员邀请，并在同一事务内写入家庭成员关系与用户上下文。
     * @param InviteId 待审批动作 ID。
     * @param Username 当前登录用户名，必须是邀请目标用户。
     * @param OutGroupId 兼容输出：成功后返回该家庭的首个账本 ID；家庭暂无账本时返回 0。
     * @param OutError 失败原因；成功时清空。
     * @return 邀请状态、家庭成员关系和当前家庭/账本上下文全部提交成功时返回 true。
     */
    bool AcceptMemberInvite(int InviteId,
                            const std::string& Username,
                            int& OutGroupId,
                            std::string& OutError);

    /**
     * @brief 拒绝成员邀请。
     * @param InviteId 待审批动作 ID。
     * @param Username 当前登录用户名，必须是邀请目标用户。
     * @param OutError 失败原因。
     * @return 状态更新成功返回 true。
     */
    bool RejectMemberInvite(int InviteId,
                            const std::string& Username,
                            std::string& OutError);

    /**
     * @brief 取消已发出的成员邀请。
     * @param InviteId 待审批动作 ID。
     * @param Username 当前登录用户名，必须是邀请发起者或目标账本 owner/admin。
     * @param OutError 失败原因。
     * @return 状态更新成功返回 true。
     */
    bool CancelMemberInvite(int InviteId,
                            const std::string& Username,
                            std::string& OutError);

    /**
     * @brief 清理过期待审批动作。
     * @param Now 当前 Unix 秒。
     * @return 被标记为 expired 的记录数量。
     */
    int CleanupExpiredPendingActions(int64_t Now);

    // ---- 会话管理 ----
    bool LoadAuthSessions(std::vector<FLedgerAuthSession>& OutSessions);
    bool UpsertAuthSession(const std::string& Token,
                           const std::string& Username,
                           int64_t CreatedAt);
    bool DeleteAuthSession(const std::string& Token);
    bool DeleteAuthSessionsByTokens(const std::vector<std::string>& Tokens);

    // ---- 家庭与账本领域 ----
    /**
     * @brief 创建家庭，并将创建者原子地写入为 owner。
     * @param Name 家庭名称，不能为空。
     * @param CreatorUsername 创建者用户名，必须是已存在且激活的用户。
     * @param OutFamilyId 输出新家庭 ID；失败时重置为 0。
     * @param OutError 输出失败原因；成功时清空。
     * @return 家庭及 owner 成员关系均提交成功时返回 true。
     * @note 该函数不会创建账本或默认分类；成功后会把用户 current_family_id 指向新家庭，
     *       并清空 current_ledger_id。任一步失败都会回滚，不留下孤立家庭。
     */
    bool CreateFamily(const std::string& Name,
                      const std::string& CreatorUsername,
                      int& OutFamilyId,
                      std::string& OutError);

    /**
     * @brief 查询用户加入的全部家庭及其家庭角色。
     * @param Username 用户名。
     * @param OutJson 输出家庭 JSON 数组，并标记当前家庭。
     * @return 查询成功返回 true；用户不存在时返回空数组。
     * @note 若当前家庭无效，会选择用户加入的首个家庭并修复 current_family_id。
     */
    bool GetUserFamilies(const std::string& Username, std::string& OutJson);

    /**
     * @brief 创建家庭账本，并在同一事务内生成账本默认分类。
     * @param FamilyId 所属家庭 ID。
     * @param Name 账本名称，不能为空且在家庭内唯一。
     * @param CreatorUsername 创建者用户名，必须是该家庭 owner。
     * @param OutLedgerId 输出新账本 ID；失败时重置为 0。
     * @param OutError 输出失败原因；成功时清空。
     * @return 账本、默认分类和当前上下文均提交成功时返回 true。
     * @note 成功后会同时设置 current_family_id 与 current_ledger_id；任一步失败全部回滚。
     */
    bool CreateLedger(int FamilyId,
                      const std::string& Name,
                      const std::string& CreatorUsername,
                      int& OutLedgerId,
                      std::string& OutError);

    /**
     * @brief 查询家庭下的全部账本。
     * @param FamilyId 家庭 ID。
     * @param Username 当前用户，必须是家庭成员。
     * @param OutJson 输出账本 JSON 数组，并标记当前账本。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权及查询成功返回 true。
     */
    bool GetFamilyLedgers(int FamilyId,
                          const std::string& Username,
                          std::string& OutJson,
                          std::string& OutError);

    /** @brief 获取并修复用户当前家庭上下文；无可用家庭时返回 false 且输出 0。 */
    bool GetCurrentFamilyId(const std::string& Username, int& OutFamilyId);
    /** @brief 设置当前家庭；用户必须是目标家庭成员，并同步修复当前账本。 */
    bool SetCurrentFamilyId(const std::string& Username,
                            int FamilyId,
                            std::string& OutError);
    /** @brief 获取并修复用户当前账本；账本必须属于用户已加入的家庭。 */
    bool GetCurrentLedgerId(const std::string& Username, int& OutLedgerId);
    /** @brief 设置当前账本；用户必须是账本所属家庭成员，并同步 current_family_id。 */
    bool SetCurrentLedgerId(const std::string& Username,
                            int LedgerId,
                            std::string& OutError);

    /** @brief 判断用户是否为家庭成员；只认 family_members，不读取 UI 上下文。 */
    bool IsFamilyMember(int FamilyId, const std::string& Username);
    /** @brief 判断用户是否为家庭 owner；系统 admin 不会被隐式视为家庭 owner。 */
    bool IsFamilyOwner(int FamilyId, const std::string& Username);
    /** @brief 查询账本所属家庭 ID；账本不存在时输出 0 并返回 false。 */
    bool GetLedgerFamilyId(int LedgerId, int& OutFamilyId);
    /** @brief 判断用户是否可访问账本；访问权由账本所属家庭成员关系继承。 */
    bool CanAccessLedger(int LedgerId, const std::string& Username);
    /** @brief 判断用户是否可管理账本及其分类；仅账本所属家庭 owner 可管理。 */
    bool CanManageLedger(int LedgerId, const std::string& Username);

    /**
     * @brief 由家庭 owner 直接添加成员，主要供注册邀请事务内部复用。
     * @note 常规“邀请已有用户”流程应使用 pending action，不应直接调用本接口。
     */
    bool AddFamilyMember(int FamilyId,
                         const std::string& Username,
                         const std::string& Role,
                         const std::string& RequestedByUsername,
                         std::string& OutError);
    /** @brief 修改家庭角色；禁止把最后一个 owner 降级为 member。 */
    bool UpdateFamilyMemberRole(int FamilyId,
                                const std::string& Username,
                                const std::string& Role,
                                const std::string& RequestedByUsername,
                                std::string& OutError);
    /** @brief 移除家庭成员；禁止移除最后一个 owner，检查与删除位于同一事务。 */
    bool RemoveFamilyMember(int FamilyId,
                            const std::string& Username,
                            const std::string& RequestedByUsername,
                            std::string& OutError);
    /** @brief 查询家庭成员；调用者必须是该家庭成员。 */
    bool GetFamilyMembers(int FamilyId,
                          const std::string& RequestedByUsername,
                          std::string& OutJson,
                          std::string& OutError);

    // ---- 旧家庭组兼容接口（调用方迁移完成后删除） ----
    bool CreateGroup(const std::string& Name, const std::string& CreatorUsername,
                     int& OutGroupId);
    bool GetUserGroups(const std::string& Username, std::string& OutJson);
    bool DeleteGroup(int GroupId, const std::string& Username,
                     std::string& OutError);
    bool GetCurrentGroupId(const std::string& Username, int& OutGroupId);
    bool SetCurrentGroupId(const std::string& Username, int GroupId,
                           std::string& OutError);
    bool GetGroupMembers(int GroupId, std::string& OutJson);

    // ---- 账本分类管理 ----
    /**
     * @brief 在指定账本中创建分类。
     * @param LedgerId 目标账本 ID。
     * @param Name 分类名称，不能为空。
     * @param Type 分类类型，仅允许 expense/income；子分类会强制继承父分类类型。
     * @param ParentId 父分类 ID；0 表示一级分类，非 0 时父分类必须属于同一账本且为一级分类。
     * @param SortOrder 同级显示顺序。
     * @param RequestedByUsername 操作用户名，必须是账本所属家庭 owner。
     * @param OutId 输出新分类 ID；失败时重置为 0。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权、约束校验及写入均成功时返回 true。
     */
    bool CreateLedgerCategory(int LedgerId,
                              const std::string& Name,
                              const std::string& Type,
                              int ParentId,
                              int SortOrder,
                              const std::string& RequestedByUsername,
                              int& OutId,
                              std::string& OutError);

    /**
     * @brief 查询账本分类树的扁平 JSON 数据。
     * @param LedgerId 目标账本 ID。
     * @param Type 可选类型过滤，仅允许空值、expense 或 income。
     * @param RequestedByUsername 操作用户名，必须是账本所属家庭成员。
     * @param OutJson 输出分类 JSON 数组；失败时输出空数组。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权和查询均成功时返回 true。
     */
    bool GetLedgerCategories(int LedgerId,
                             const std::string& Type,
                             const std::string& RequestedByUsername,
                             std::string& OutJson,
                             std::string& OutError);

    /** @brief 查询分类所属账本 ID；分类不存在时返回 false 且输出 0。 */
    bool GetCategoryLedgerId(int CategoryId, int& OutLedgerId);

    /**
     * @brief 修改分类；仅账本所属家庭 owner 可操作。
     * @note 禁止分类成为自身子分类；有子分类的一级分类不能被改为子分类。
     */
    bool UpdateLedgerCategory(int CategoryId,
                              const std::string& Name,
                              const std::string& Type,
                              int ParentId,
                              int SortOrder,
                              const std::string& RequestedByUsername,
                              std::string& OutError);

    /**
     * @brief 删除分类；仅账本所属家庭 owner 可操作。
     * @note 兜底分类和仍有子分类的分类不可删除；关联流水会在同一事务内迁移到同账本兜底分类。
     */
    bool DeleteLedgerCategory(int CategoryId,
                              const std::string& RequestedByUsername,
                              std::string& OutError);

    // ---- 账本流水管理 ----
    /**
     * @brief 在指定账本中创建一条普通流水。
     * @param LedgerId 目标账本 ID，必须存在。
     * @param CategoryId 分类 ID，必须属于目标账本，且类型必须与 Type 一致。
     * @param Amount 流水金额，必须大于 0。
     * @param Type 流水类型，仅允许 expense 或 income。
     * @param Description 流水说明，可为空。
     * @param CreatedByUsername 创建者用户名，必须是账本所属家庭的有效成员。
     * @param Date 流水日期，不能为空；当前接口不隐式替换调用方日期。
     * @param OutId 输出新流水 ID；失败时重置为 0。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权、分类隔离校验和数据库写入全部成功时返回 true。
     * @note 该接口只创建普通流水，不设置语音或导入来源字段。
     */
    bool CreateLedgerTransaction(int LedgerId,
                                 int CategoryId,
                                 double Amount,
                                 const std::string& Type,
                                 const std::string& Description,
                                 const std::string& CreatedByUsername,
                                 const std::string& Date,
                                 int& OutId,
                                 std::string& OutError);

    /**
     * @brief 查询指定账本的流水列表。
     * @param LedgerId 目标账本 ID。
     * @param RequestedByUsername 查询用户，必须是账本所属家庭成员。
     * @param DateFrom 可选起始日期，空字符串表示不限制。
     * @param DateTo 可选结束日期，空字符串表示不限制。
     * @param CategoryId 可选精确分类 ID；大于 0 时优先于 ParentCategoryId。
     * @param Type 可选流水类型，仅允许空值、expense 或 income。
     * @param Offset 分页偏移量；负值会归一化为 0。
     * @param Limit 分页大小；非正数或超过 200 时归一化为 50。
     * @param OutJson 输出流水 JSON 数组；每条记录包含 ledgerId、canEdit 和 canDelete。
     * @param OutTotal 输出过滤条件下的总记录数；失败时重置为 0。
     * @param OutError 输出失败原因；成功时清空。
     * @param ParentCategoryId 可选父分类 ID，用于查询父分类及其直接子分类流水。
     * @param SortBy 排序字段白名单，仅 amount 表示按金额，其余值按日期排序。
     * @param SortOrder 排序方向，仅 asc 表示升序，其余值按降序处理。
     * @return 授权、过滤条件校验和数据库查询全部成功时返回 true。
     * @note canEdit/canDelete 按请求用户实时计算：家庭 owner 可管理全部流水，member 仅可管理自己创建的流水。
     */
    bool GetLedgerTransactions(int LedgerId,
                               const std::string& RequestedByUsername,
                               const std::string& DateFrom,
                               const std::string& DateTo,
                               int CategoryId,
                               const std::string& Type,
                               int Offset,
                               int Limit,
                               std::string& OutJson,
                               int& OutTotal,
                               std::string& OutError,
                               int ParentCategoryId = 0,
                               const std::string& SortBy = "",
                               const std::string& SortOrder = "desc");

    /**
     * @brief 查询流水所属账本 ID。
     * @param TransactionId 流水 ID。
     * @param OutLedgerId 输出账本 ID；流水不存在或查询失败时重置为 0。
     */
    bool GetTransactionLedgerId(int TransactionId, int& OutLedgerId);

    /**
     * @brief 修改一条账本流水。
     * @param TransactionId 目标流水 ID。
     * @param CategoryId 新分类 ID，必须与目标流水属于同一账本，且类型必须与 Type 一致。
     * @param Amount 新金额，必须大于 0。
     * @param Type 新类型，仅允许 expense 或 income。
     * @param Description 新说明，可为空。
     * @param Date 新流水日期，不能为空。
     * @param RequestedByUsername 操作用户名；家庭 owner 可修改全部流水，member 只能修改自己创建的流水。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权、账本隔离校验和更新全部成功时返回 true。
     * @note 不改变流水创建者、语音来源或导入来源字段。
     */
    bool UpdateLedgerTransaction(int TransactionId,
                                 int CategoryId,
                                 double Amount,
                                 const std::string& Type,
                                 const std::string& Description,
                                 const std::string& Date,
                                 const std::string& RequestedByUsername,
                                 std::string& OutError);

    /**
     * @brief 删除一条账本流水。
     * @param TransactionId 目标流水 ID。
     * @param RequestedByUsername 操作用户名；家庭 owner 可删除全部流水，member 只能删除自己创建的流水。
     * @param OutError 输出失败原因；成功时清空。
     * @return 授权检查和删除均成功时返回 true。
     */
    bool DeleteLedgerTransaction(int TransactionId,
                                 const std::string& RequestedByUsername,
                                 std::string& OutError);

    /**
     * @brief 按导入来源键在账本内新增或更新流水。
     * @param LedgerId 目标账本 ID；创建者必须是家庭成员。
     * @note 去重作用域固定为 ledger_id + import_source + import_source_key。
     */
    bool UpsertImportedTransaction(int LedgerId,
                                   const FLedgerImportedTransaction& Entry,
                                   const std::string& CreatedBy,
                                   bool& bOutInserted,
                                   int& OutId,
                                   std::string& OutError);

    // ---- 语音识别录入账目 ----
    /**
     * @brief 在账本内创建待解析语音流水。
     * @param LedgerId 目标账本 ID；创建者必须是家庭成员。
     * @param VoiceText 原始语音文本，不能为空。
     */
    bool CreateVoiceInputTransaction(int LedgerId, const std::string& VoiceText,
                                     const std::string& CreatedBy,
                                     int& OutId, std::string& OutError);
    bool RequeueVoiceInputTransaction(int TransactionId,
                                      const std::string& Username,
                                      std::string& OutError);
    bool GetPendingVoiceInputTransactions(std::vector<FLedgerVoicePendingItem>& OutItems,
                                          int Limit = 5);
    bool MarkVoiceInputProcessing(int TransactionId);
    bool MarkVoiceInputFailed(int TransactionId, const std::string& Error);
    bool ApplyVoiceParseResult(int TransactionId,
                               const std::vector<FLedgerParsedVoiceEntry>& Entries,
                               std::string& OutError);

    // ---- 账单导入任务 ----
    /**
     * @brief 创建账本导入任务。
     * @param LedgerId 目标账本 ID；Username 必须是家庭成员。
     * @return 创建成功返回任务 ID；失败返回 0。
     */
    int CreateBillImportTask(int LedgerId, const std::string& Username,
                             const std::string& Filename,
                             const std::string& FileContent);

    // 获取待处理的导入任务（按 id 升序）
    bool GetPendingBillImportTasks(std::vector<FBillImportTaskItem>& OutItems, int Limit = 1);

    // 标记任务处理中
    bool MarkBillImportProcessing(int TaskId);

    // 标记任务完成（写入统计结果）
    bool MarkBillImportDone(int TaskId, int TotalRows, int ImportedRows,
                            int InsertedRows, int UpdatedRows, int SkippedRows,
                            const std::string& SourceType,
                            const std::string& Message);

    // 标记任务失败
    bool MarkBillImportFailed(int TaskId, const std::string& Error);

    // 查询任务状态
    bool GetBillImportTaskStatus(int TaskId, FBillImportTaskItem& OutItem);

    // ---- 账本统计 ----
    /**
     * @brief 查询指定账本的收支汇总、分类聚合和每日趋势。
     * @param LedgerId 目标账本 ID。
     * @param RequestedByUsername 请求用户名，必须是账本所属家庭成员。
     * @param DateFrom 可选起始日期，按 `transaction_date >= DateFrom` 过滤。
     * @param DateTo 可选结束日期，按 `transaction_date <= DateTo` 过滤。
     * @param GroupBy 保留的统计粒度参数；当前分类和每日趋势响应结构保持固定。
     * @param OutJson 成功时输出统计 JSON；失败时重置为空对象。
     * @param OutError 失败时输出明确原因；成功时清空。
     * @param ParentCategoryId 可选一级分类 ID；大于 0 时只聚合该分类的直接子分类，且分类必须属于目标账本。
     * @return 授权、参数校验及全部统计查询成功时返回 true。
     * @note 日期和分类参数均通过 SQLite 参数绑定，不参与 SQL 字符串拼接。
     */
    bool GetLedgerStats(int LedgerId,
                        const std::string& RequestedByUsername,
                        const std::string& DateFrom,
                        const std::string& DateTo,
                        const std::string& GroupBy,
                        std::string& OutJson,
                        std::string& OutError,
                        int ParentCategoryId = 0);

    // ---- 权限工具 ----
    bool IsGroupMember(int GroupId, const std::string& Username);
    bool IsGroupAdmin(int GroupId, const std::string& Username);
    int  GetUserId(const std::string& Username);  // 返回 -1 表示不存在
    bool IsUserAdmin(const std::string& Username);

private:
    bool SerializeSystemSettingsToJson(std::string& OutJson,
                                       const std::vector<FSystemSettingItem>& Items) const;
    bool LoadSystemSettingRows(std::vector<FSystemSettingItem>& OutItems);

    // SQLite 数据库句柄
    sqlite3* m_Db = nullptr;

    // 线程安全锁（SQLite 本身在多线程模式下安全，但加锁更保险）
    mutable std::mutex m_Mutex;

    // 建表
    bool CreateTables();

    /**
     * @brief 为指定账本复制默认分类模板。
     * @param LedgerId 目标账本 ID，必须属于已存在账本。
     * @return 所有默认分类均成功写入时返回 true；任一条失败返回 false。
     * @note 调用方必须已持有 m_Mutex；该函数不自行开启或提交事务，便于 CreateLedger 将
     *       账本与分类纳入同一原子事务。
     */
    bool CreateDefaultCategories(int LedgerId);

    // 判断是否为账本级兜底分类：其他支出 / 其他收入。
    // 这两个分类用于导入、语音、删除分类时的最终回退，不允许删除。
    bool IsProtectedFallbackCategoryName(const std::string& Name,
                                         const std::string& Type) const;

    // 安全执行 SQL（无回调）
    bool ExecuteSql(const std::string& Sql);

    // JSON 转义工具
    static std::string EscapeJsonString(const std::string& S);
    static std::string ExtractPermsJson(const std::string& RawPerms);
};
