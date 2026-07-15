// RuntimeSettings.h — 运行时系统参数核心库
//
// 该模块只负责系统参数的定义、校验、运行时快照与观察者分发，不直接依赖 Web、语音、账单导入等业务实现。
// 所有业务模块应通过 IRuntimeSettingObserver 响应参数变化，避免把实时生效逻辑堆积在 WebServer 中。

#pragma once

#include "LedgerManager.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief 系统参数运行时生效模式。
 *
 * 该枚举直接决定管理后台更新参数后的返回协议与业务侧处理策略。
 */
enum class ERuntimeSettingApplyMode
{
    /** 参数保存成功后可立即影响后续业务请求。 */
    Immediate,

    /** 参数无需重启进程，但需要模块级软重载或下次请求重新读取快照。 */
    SoftReload,

    /** 参数已持久化，但当前进程无法无损应用，必须重启服务后生效。 */
    RestartRequired,

    /** 参数不支持由管理后台修改，仅保留历史兼容或内部使用。 */
    Unsupported
};

/**
 * @brief 系统参数值类型。
 */
enum class ERuntimeSettingValueType
{
    /** UTF-8 字符串。 */
    String,

    /** 32 位整数。 */
    Int,

    /** 双精度浮点数。 */
    Double,

    /** 布尔值，支持 true/false/1/0/yes/no/on/off。 */
    Bool
};

class RuntimeSettingManager;

/**
 * @brief 将运行时生效模式转换为前端协议字符串。
 * @param Mode 生效模式枚举。
 * @return 小驼峰协议字符串，例如 immediate、restartRequired。
 */
std::string RuntimeSettingApplyModeToString(ERuntimeSettingApplyMode Mode);

/**
 * @brief 将参数值类型转换为数据库和前端使用的字符串。
 * @param Type 值类型枚举。
 * @return string/int/double/bool。
 */
std::string RuntimeSettingValueTypeToString(ERuntimeSettingValueType Type);

/**
 * @brief 从数据库 value_type 字符串解析强类型枚举。
 * @param RawType 数据库或前端传入的类型字符串。
 * @return 无法识别时返回 String。
 */
ERuntimeSettingValueType ParseRuntimeSettingValueType(const std::string& RawType);

/**
 * @brief 系统参数定义元数据。
 *
 * 定义表是运行时参数系统的唯一语义来源。新增参数必须先注册定义，再允许后台保存与应用。
 */
struct FRuntimeSettingDefinition
{
    /** 参数键，必须全局唯一且非空。 */
    std::string Key;

    /** 默认原始值；删除参数或数据库缺失时使用。 */
    std::string DefaultValue;

    /** 参数值类型。 */
    ERuntimeSettingValueType ValueType = ERuntimeSettingValueType::String;

    /** 管理后台分类。 */
    std::string Category = "general";

    /** 中文参数说明，用于后台展示。 */
    std::string Description;

    /** 是否为敏感值；敏感值不应在日志中明文输出。 */
    bool bSensitive = false;

    /** 参数生效模式。 */
    ERuntimeSettingApplyMode ApplyMode = ERuntimeSettingApplyMode::Immediate;

    /** 整数最小值，仅 bHasIntRange=true 时生效。 */
    int MinInt = 0;

    /** 整数最大值，仅 bHasIntRange=true 时生效。 */
    int MaxInt = 0;

    /** 是否启用整数范围校验。 */
    bool bHasIntRange = false;

    /** 浮点最小值，仅 bHasDoubleRange=true 时生效。 */
    double MinDouble = 0.0;

    /** 浮点最大值，仅 bHasDoubleRange=true 时生效。 */
    double MaxDouble = 0.0;

    /** 是否启用浮点范围校验。 */
    bool bHasDoubleRange = false;
};

/**
 * @brief 单个参数运行时快照。
 */
struct FRuntimeSettingValue
{
    /** 参数键。 */
    std::string Key;

    /** 当前生效的原始字符串值。 */
    std::string RawValue;

    /** 参数定义副本，便于无锁回调期间读取元数据。 */
    FRuntimeSettingDefinition Definition;

    /** @return 按字符串返回当前值。 */
    std::string AsString() const;

    /** @param DefaultValue 解析失败时返回的默认值。 */
    int AsInt(int DefaultValue = 0) const;

    /** @param DefaultValue 解析失败时返回的默认值。 */
    double AsDouble(double DefaultValue = 0.0) const;

    /** @param DefaultValue 解析失败时返回的默认值。 */
    bool AsBool(bool DefaultValue = false) const;
};

/**
 * @brief 运行时参数变更事件。
 */
struct FRuntimeSettingChangeEvent
{
    /** 参数键。 */
    std::string SettingKey;

    /** 变更前原始值。 */
    std::string OldValue;

    /** 变更后原始值。 */
    std::string NewValue;

    /** 最终运行时有效值；删除时为定义默认值。 */
    std::string EffectiveValue;

    /** 值类型字符串。 */
    std::string ValueType;

    /** 参数分类。 */
    std::string Category;

    /** 生效模式。 */
    ERuntimeSettingApplyMode ApplyMode = ERuntimeSettingApplyMode::Immediate;

    /** 是否为删除事件。 */
    bool bDeleted = false;

    /** 值是否通过定义校验。 */
    bool bValid = true;

    /** 是否需要重启服务。 */
    bool bRestartRequired = false;

    /** 更新人。 */
    std::string UpdatedBy;

    /** 更新时间 Unix 秒。 */
    int64_t UpdatedAtUnix = 0;

    /** 面向日志和前端的中文或英文提示。 */
    std::string Message;
};

/**
 * @brief 参数应用结果，用于观察者返回与接口序列化。
 */
struct FRuntimeSettingApplyResult
{
    /** 操作是否成功。 */
    bool bOk = true;

    /** 当前运行实例是否已应用。 */
    bool bApplied = false;

    /** 是否需要重启后生效。 */
    bool bRestartRequired = false;

    /** 生效模式。 */
    ERuntimeSettingApplyMode ApplyMode = ERuntimeSettingApplyMode::Immediate;

    /** 最终有效值。 */
    std::string EffectiveValue;

    /** 返回给前端的说明。 */
    std::string Message;
};

/**
 * @brief 追加参数应用结果 JSON 字段。
 * @param Stream 输出流，调用方负责已经写入对象起始内容。
 * @param Result 参数应用结果。
 * @sideeffect 向 Stream 写入逗号开头的 JSON 字段片段。
 */
void AppendRuntimeSettingApplyResultJson(std::ostream& Stream,
                                         const FRuntimeSettingApplyResult& Result);

/**
 * @brief 运行时参数观察者接口。
 *
 * 观察者由业务模块持有或实现，RuntimeSettingManager 仅保存非拥有型指针。
 * 调用方必须保证观察者生命周期长于注册有效期。
 */
class IRuntimeSettingObserver
{
public:
    virtual ~IRuntimeSettingObserver() = default;

    /** @return 观察者名称，用于日志定位。 */
    virtual std::string GetObserverName() const = 0;

    /**
     * @brief 判断当前观察者是否处理指定参数。
     * @param Key 参数键。
     * @return true 表示 RuntimeSettingManager 会把该参数事件派发给该观察者。
     */
    virtual bool CanHandleSetting(const std::string& Key) const = 0;

    /**
     * @brief 响应运行时参数变更。
     * @param Event 参数变更事件。
     * @param Manager 参数管理器，可用于读取完整快照。
     * @return 当前观察者的应用结果。
     * @sideeffect 允许观察者修改自身业务模块运行态，例如重载语音配置或裁剪会话。
     */
    virtual FRuntimeSettingApplyResult OnRuntimeSettingChanged(
        const FRuntimeSettingChangeEvent& Event,
        const RuntimeSettingManager& Manager) = 0;
};

/**
 * @brief 运行时系统参数中枢。
 *
 * 该类负责参数定义、快照、校验和观察者分发。内部锁只保护定义表与快照；调用观察者前会释放锁，避免跨模块死锁。
 */
class RuntimeSettingManager
{
public:
    RuntimeSettingManager() = default;
    ~RuntimeSettingManager() = default;

    RuntimeSettingManager(const RuntimeSettingManager&) = delete;
    RuntimeSettingManager& operator=(const RuntimeSettingManager&) = delete;

    /** @brief 清空定义与快照，通常仅用于重新初始化。 */
    void Reset();

    /** @brief 注册当前工程内置的全部系统参数定义。 */
    void RegisterDefaultDefinitions();

    /**
     * @brief 注册或覆盖单个参数定义。
     * @param Definition 参数定义，Key 必须非空。
     * @return true 表示注册成功。
     */
    bool RegisterDefinition(const FRuntimeSettingDefinition& Definition);

    /**
     * @brief 从数据库加载参数并生成运行时快照。
     * @param Ledger 已初始化的账本数据库管理器。
     * @return true 表示加载完成；false 表示数据库访问失败。
     */
    bool LoadFromLedger(LedgerManager& Ledger);

    /**
     * @brief 注册参数观察者。
     * @param Observer 非拥有型观察者指针，不能为空且生命周期必须覆盖注册期。
     */
    void RegisterObserver(IRuntimeSettingObserver* Observer);

    /** @brief 清空观察者列表，不影响定义和快照。 */
    void ClearObservers();

    /**
     * @brief 保存前校验原始值。
     * @param Key 参数键。
     * @param RawValue 待保存原始字符串。
     * @param OutMessage 失败时写入原因，成功时写入简短说明。
     * @return true 表示值合法并允许保存。
     */
    bool ValidateRawValue(const std::string& Key,
                          const std::string& RawValue,
                          std::string& OutMessage) const;

    /**
     * @brief 使用参数定义补齐数据库 item，避免前端传空元数据导致定义漂移。
     * @param InOutItem 待补齐的参数项。
     * @return true 表示找到定义并完成补齐。
     */
    bool BuildSettingItemWithDefinition(FSystemSettingItem& InOutItem) const;

    /**
     * @brief 应用数据库 upsert 事件并分发观察者。
     * @param Event LedgerManager 输出的持久化事件。
     * @return 聚合后的应用结果。
     */
    FRuntimeSettingApplyResult ApplyUpsertEvent(const FSystemSettingChangedEvent& Event);

    /**
     * @brief 应用数据库 delete 事件，快照回退到默认值并分发观察者。
     * @param Event 删除事件。
     * @return 聚合后的应用结果。
     */
    FRuntimeSettingApplyResult ApplyDeleteEvent(const FSystemSettingChangedEvent& Event);

    /** @return 指定键的字符串值；不存在时返回 DefaultValue。 */
    std::string GetString(const std::string& Key, const std::string& DefaultValue = "") const;

    /** @return 指定键的整数值；不存在或解析失败时返回 DefaultValue。 */
    int GetInt(const std::string& Key, int DefaultValue = 0) const;

    /** @return 指定键的浮点值；不存在或解析失败时返回 DefaultValue。 */
    double GetDouble(const std::string& Key, double DefaultValue = 0.0) const;

    /** @return 指定键的布尔值；不存在或解析失败时返回 DefaultValue。 */
    bool GetBool(const std::string& Key, bool DefaultValue = false) const;

    /** @return 指定键的定义；不存在时返回 false。 */
    bool GetDefinition(const std::string& Key, FRuntimeSettingDefinition& OutDefinition) const;

private:
    FRuntimeSettingApplyResult ApplyEventInternal(const FSystemSettingChangedEvent& SourceEvent,
                                                 bool bDeleted);
    FRuntimeSettingApplyResult DispatchEvent(const FRuntimeSettingChangeEvent& Event);
    bool ValidateByDefinition(const FRuntimeSettingDefinition& Definition,
                              const std::string& RawValue,
                              std::string& OutMessage) const;

private:
    mutable std::mutex m_Mutex;
    std::map<std::string, FRuntimeSettingDefinition> m_Definitions;
    std::map<std::string, FRuntimeSettingValue> m_Snapshot;
    std::vector<IRuntimeSettingObserver*> m_Observers;
};
