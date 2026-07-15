// RuntimeSettings.cpp — 运行时系统参数核心库实现

#include "RuntimeSettings.h"
#include "JsonLite.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <ostream>
#include <sstream>

namespace
{
    std::string ToLowerAscii(std::string Value)
    {
        std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return Value;
    }

    bool TryParseInt(const std::string& RawValue, int& OutValue)
    {
        try {
            size_t consumed = 0;
            int parsed = std::stoi(RawValue, &consumed);
            if (consumed != RawValue.size()) return false;
            OutValue = parsed;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool TryParseDouble(const std::string& RawValue, double& OutValue)
    {
        try {
            size_t consumed = 0;
            double parsed = std::stod(RawValue, &consumed);
            if (consumed != RawValue.size()) return false;
            OutValue = parsed;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool TryParseBool(const std::string& RawValue, bool& OutValue)
    {
        const std::string lower = ToLowerAscii(RawValue);
        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
            OutValue = true;
            return true;
        }
        if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
            OutValue = false;
            return true;
        }
        return false;
    }

    FRuntimeSettingDefinition MakeDefinition(const std::string& Key,
                                             const std::string& DefaultValue,
                                             ERuntimeSettingValueType ValueType,
                                             const std::string& Category,
                                             bool bSensitive,
                                             ERuntimeSettingApplyMode ApplyMode,
                                             const std::string& Description)
    {
        FRuntimeSettingDefinition Definition;
        Definition.Key = Key;
        Definition.DefaultValue = DefaultValue;
        Definition.ValueType = ValueType;
        Definition.Category = Category;
        Definition.bSensitive = bSensitive;
        Definition.ApplyMode = ApplyMode;
        Definition.Description = Description;
        return Definition;
    }

    void SetIntRange(FRuntimeSettingDefinition& Definition, int MinValue, int MaxValue)
    {
        Definition.bHasIntRange = true;
        Definition.MinInt = MinValue;
        Definition.MaxInt = MaxValue;
    }

    void SetDoubleRange(FRuntimeSettingDefinition& Definition, double MinValue, double MaxValue)
    {
        Definition.bHasDoubleRange = true;
        Definition.MinDouble = MinValue;
        Definition.MaxDouble = MaxValue;
    }
}

std::string RuntimeSettingApplyModeToString(ERuntimeSettingApplyMode Mode)
{
    switch (Mode) {
    case ERuntimeSettingApplyMode::Immediate:
        return "immediate";
    case ERuntimeSettingApplyMode::SoftReload:
        return "softReload";
    case ERuntimeSettingApplyMode::RestartRequired:
        return "restartRequired";
    case ERuntimeSettingApplyMode::Unsupported:
        return "unsupported";
    }
    return "immediate";
}

std::string RuntimeSettingValueTypeToString(ERuntimeSettingValueType Type)
{
    switch (Type) {
    case ERuntimeSettingValueType::String:
        return "string";
    case ERuntimeSettingValueType::Int:
        return "int";
    case ERuntimeSettingValueType::Double:
        return "double";
    case ERuntimeSettingValueType::Bool:
        return "bool";
    }
    return "string";
}

ERuntimeSettingValueType ParseRuntimeSettingValueType(const std::string& RawType)
{
    const std::string lower = ToLowerAscii(RawType);
    if (lower == "int" || lower == "integer") return ERuntimeSettingValueType::Int;
    if (lower == "double" || lower == "float" || lower == "number") return ERuntimeSettingValueType::Double;
    if (lower == "bool" || lower == "boolean") return ERuntimeSettingValueType::Bool;
    return ERuntimeSettingValueType::String;
}

std::string FRuntimeSettingValue::AsString() const
{
    return RawValue;
}

int FRuntimeSettingValue::AsInt(int DefaultValue) const
{
    int parsed = DefaultValue;
    return TryParseInt(RawValue, parsed) ? parsed : DefaultValue;
}

double FRuntimeSettingValue::AsDouble(double DefaultValue) const
{
    double parsed = DefaultValue;
    return TryParseDouble(RawValue, parsed) ? parsed : DefaultValue;
}

bool FRuntimeSettingValue::AsBool(bool DefaultValue) const
{
    bool parsed = DefaultValue;
    return TryParseBool(RawValue, parsed) ? parsed : DefaultValue;
}

void AppendRuntimeSettingApplyResultJson(std::ostream& Stream,
                                         const FRuntimeSettingApplyResult& Result)
{
    Stream << ",\"applyMode\":\"" << JsonLite::EscapeString(RuntimeSettingApplyModeToString(Result.ApplyMode)) << "\""
           << ",\"applied\":" << (Result.bApplied ? "true" : "false")
           << ",\"restartRequired\":" << (Result.bRestartRequired ? "true" : "false")
           << ",\"effectiveValue\":\"" << JsonLite::EscapeString(Result.EffectiveValue) << "\""
           << ",\"message\":\"" << JsonLite::EscapeString(Result.Message) << "\"";
}

void RuntimeSettingManager::Reset()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Definitions.clear();
    m_Snapshot.clear();
    m_Observers.clear();
}

void RuntimeSettingManager::RegisterDefaultDefinitions()
{
    std::vector<FRuntimeSettingDefinition> Definitions;

    Definitions.push_back(MakeDefinition("server.listenIp", "0.0.0.0", ERuntimeSettingValueType::String,
        "server", false, ERuntimeSettingApplyMode::RestartRequired,
        "HTTP 服务监听地址；保存后需重启 YuanBook 服务生效。默认 0.0.0.0。"));

    FRuntimeSettingDefinition HttpPort = MakeDefinition("server.httpPort", "5080", ERuntimeSettingValueType::Int,
        "server", false, ERuntimeSettingApplyMode::RestartRequired,
        "HTTP 服务监听端口；保存后需重启 YuanBook 服务生效。默认 5080。");
    SetIntRange(HttpPort, 1, 65535);
    Definitions.push_back(HttpPort);

    Definitions.push_back(MakeDefinition("server.wwwDir", "./www", ERuntimeSettingValueType::String,
        "server", false, ERuntimeSettingApplyMode::SoftReload,
        "Web 前端静态资源目录；保存后后续静态资源请求按新目录读取。"));

    FRuntimeSettingDefinition MaxSessions = MakeDefinition("auth.maxSessionsPerUser", "5", ERuntimeSettingValueType::Int,
        "auth", false, ERuntimeSettingApplyMode::Immediate,
        "每个用户最多保留的登录会话数，<=0 表示不限制；保存后立即裁剪超额会话。");
    SetIntRange(MaxSessions, -1, 100000);
    Definitions.push_back(MaxSessions);

    FRuntimeSettingDefinition InviteExpires = MakeDefinition("ledger.memberInviteExpiresSec", "86400", ERuntimeSettingValueType::Int,
        "ledger", false, ERuntimeSettingApplyMode::Immediate,
        "指定账号成员邀请有效期秒数；保存后新创建邀请立即使用该值。默认 86400，即 24 小时。已创建邀请不追溯修改。");
    SetIntRange(InviteExpires, 60, 31536000);
    Definitions.push_back(InviteExpires);

    FRuntimeSettingDefinition RegistrationInviteExpires = MakeDefinition(
        "registrationInvite.expiresSec",
        "86400",
        ERuntimeSettingValueType::Int,
        "registrationInvite",
        false,
        ERuntimeSettingApplyMode::Immediate,
        "注册邀请有效期秒数；保存后新创建邀请立即使用该值。默认 86400，即 24 小时；已创建邀请不追溯修改。");
    SetIntRange(RegistrationInviteExpires, 60, 31536000);
    Definitions.push_back(RegistrationInviteExpires);

    Definitions.push_back(MakeDefinition(
        "registrationInvite.allowNonAdminIndependentUserInvite",
        "false",
        ERuntimeSettingValueType::Bool,
        "registrationInvite",
        false,
        ERuntimeSettingApplyMode::Immediate,
        "兼容参数：是否允许非系统管理员的家庭 owner 创建独立用户注册邀请。当前产品入口仅向系统管理员开放。"));

    Definitions.push_back(MakeDefinition("voiceLedger.enabled", "true", ERuntimeSettingValueType::Bool,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "是否启用语音记账能力；保存后立即影响后续语音与账单 AI 分类请求。"));
    Definitions.push_back(MakeDefinition("voiceLedger.apiKey", "", ERuntimeSettingValueType::String,
        "voiceLedger", true, ERuntimeSettingApplyMode::Immediate,
        "DeepSeek API Key，可为空；为空时语音记账能力处于不可用状态。"));
    Definitions.push_back(MakeDefinition("voiceLedger.endpoint", "https://api.deepseek.com/chat/completions", ERuntimeSettingValueType::String,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "语音记账模型调用地址；保存后新请求立即使用。"));
    Definitions.push_back(MakeDefinition("voiceLedger.model", "deepseek-chat", ERuntimeSettingValueType::String,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "语音记账模型名称；保存后新请求立即使用。"));
    Definitions.push_back(MakeDefinition("voiceLedger.curlPath", "curl", ERuntimeSettingValueType::String,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "调用外部 curl 的可执行路径；保存后新请求立即使用。"));

    FRuntimeSettingDefinition Timeout = MakeDefinition("voiceLedger.timeoutSec", "60", ERuntimeSettingValueType::Int,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "语音记账请求超时秒数；保存后新请求立即使用。");
    SetIntRange(Timeout, 1, 600);
    Definitions.push_back(Timeout);

    FRuntimeSettingDefinition Temperature = MakeDefinition("voiceLedger.temperature", "0.1", ERuntimeSettingValueType::Double,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "语音记账模型 temperature 参数；保存后新请求立即使用。");
    SetDoubleRange(Temperature, 0.0, 2.0);
    Definitions.push_back(Temperature);

    FRuntimeSettingDefinition MaxTokens = MakeDefinition("voiceLedger.maxTokens", "512", ERuntimeSettingValueType::Int,
        "voiceLedger", false, ERuntimeSettingApplyMode::Immediate,
        "语音记账模型最大 token 数；保存后新请求立即使用。");
    SetIntRange(MaxTokens, 1, 32768);
    Definitions.push_back(MaxTokens);

    for (const FRuntimeSettingDefinition& Definition : Definitions) {
        RegisterDefinition(Definition);
    }
}

bool RuntimeSettingManager::RegisterDefinition(const FRuntimeSettingDefinition& Definition)
{
    if (Definition.Key.empty()) return false;

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Definitions[Definition.Key] = Definition;

    if (m_Snapshot.find(Definition.Key) == m_Snapshot.end()) {
        FRuntimeSettingValue Value;
        Value.Key = Definition.Key;
        Value.RawValue = Definition.DefaultValue;
        Value.Definition = Definition;
        m_Snapshot[Definition.Key] = Value;
    }
    return true;
}

bool RuntimeSettingManager::LoadFromLedger(LedgerManager& Ledger)
{
    std::vector<FRuntimeSettingDefinition> definitions;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        definitions.reserve(m_Definitions.size());
        for (const auto& Pair : m_Definitions) {
            definitions.push_back(Pair.second);
        }
    }

    std::map<std::string, FRuntimeSettingValue> loadedSnapshot;
    for (const FRuntimeSettingDefinition& Definition : definitions) {
        std::string value;
        if (!Ledger.GetSystemSettingValue(Definition.Key, value)) {
            value = Definition.DefaultValue;
        }

        std::string message;
        if (!ValidateByDefinition(Definition, value, message)) {
            value = Definition.DefaultValue;
        }

        FRuntimeSettingValue RuntimeValue;
        RuntimeValue.Key = Definition.Key;
        RuntimeValue.RawValue = value;
        RuntimeValue.Definition = Definition;
        loadedSnapshot[Definition.Key] = RuntimeValue;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Snapshot = loadedSnapshot;
    return true;
}

void RuntimeSettingManager::RegisterObserver(IRuntimeSettingObserver* Observer)
{
    if (!Observer) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (std::find(m_Observers.begin(), m_Observers.end(), Observer) == m_Observers.end()) {
        m_Observers.push_back(Observer);
    }
}

void RuntimeSettingManager::ClearObservers()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Observers.clear();
}

bool RuntimeSettingManager::ValidateRawValue(const std::string& Key,
                                             const std::string& RawValue,
                                             std::string& OutMessage) const
{
    FRuntimeSettingDefinition Definition;
    if (!GetDefinition(Key, Definition)) {
        OutMessage = "Unknown system setting";
        return false;
    }
    return ValidateByDefinition(Definition, RawValue, OutMessage);
}

bool RuntimeSettingManager::BuildSettingItemWithDefinition(FSystemSettingItem& InOutItem) const
{
    FRuntimeSettingDefinition Definition;
    if (!GetDefinition(InOutItem.SettingKey, Definition)) {
        return false;
    }

    InOutItem.ValueType = RuntimeSettingValueTypeToString(Definition.ValueType);
    InOutItem.Category = Definition.Category;
    InOutItem.bSensitive = Definition.bSensitive;
    InOutItem.Description = Definition.Description;
    return true;
}

FRuntimeSettingApplyResult RuntimeSettingManager::ApplyUpsertEvent(const FSystemSettingChangedEvent& Event)
{
    return ApplyEventInternal(Event, false);
}

FRuntimeSettingApplyResult RuntimeSettingManager::ApplyDeleteEvent(const FSystemSettingChangedEvent& Event)
{
    return ApplyEventInternal(Event, true);
}

std::string RuntimeSettingManager::GetString(const std::string& Key, const std::string& DefaultValue) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Snapshot.find(Key);
    return it != m_Snapshot.end() ? it->second.AsString() : DefaultValue;
}

int RuntimeSettingManager::GetInt(const std::string& Key, int DefaultValue) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Snapshot.find(Key);
    return it != m_Snapshot.end() ? it->second.AsInt(DefaultValue) : DefaultValue;
}

double RuntimeSettingManager::GetDouble(const std::string& Key, double DefaultValue) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Snapshot.find(Key);
    return it != m_Snapshot.end() ? it->second.AsDouble(DefaultValue) : DefaultValue;
}

bool RuntimeSettingManager::GetBool(const std::string& Key, bool DefaultValue) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Snapshot.find(Key);
    return it != m_Snapshot.end() ? it->second.AsBool(DefaultValue) : DefaultValue;
}

bool RuntimeSettingManager::GetDefinition(const std::string& Key, FRuntimeSettingDefinition& OutDefinition) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Definitions.find(Key);
    if (it == m_Definitions.end()) return false;
    OutDefinition = it->second;
    return true;
}

FRuntimeSettingApplyResult RuntimeSettingManager::ApplyEventInternal(const FSystemSettingChangedEvent& SourceEvent,
                                                                     bool bDeleted)
{
    FRuntimeSettingChangeEvent Event;
    FRuntimeSettingApplyResult Result;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto defIt = m_Definitions.find(SourceEvent.SettingKey);
        if (defIt == m_Definitions.end()) {
            Result.bOk = false;
            Result.bApplied = false;
            Result.bRestartRequired = false;
            Result.Message = "Unknown system setting";
            return Result;
        }

        const FRuntimeSettingDefinition Definition = defIt->second;
        const std::string effectiveValue = bDeleted ? Definition.DefaultValue : SourceEvent.NewValue;

        std::string validateMessage;
        if (!ValidateByDefinition(Definition, effectiveValue, validateMessage)) {
            Result.bOk = false;
            Result.bApplied = false;
            Result.ApplyMode = Definition.ApplyMode;
            Result.bRestartRequired = Definition.ApplyMode == ERuntimeSettingApplyMode::RestartRequired;
            Result.EffectiveValue = effectiveValue;
            Result.Message = validateMessage;
            return Result;
        }

        FRuntimeSettingValue RuntimeValue;
        RuntimeValue.Key = Definition.Key;
        RuntimeValue.RawValue = effectiveValue;
        RuntimeValue.Definition = Definition;
        m_Snapshot[Definition.Key] = RuntimeValue;

        Event.SettingKey = Definition.Key;
        Event.OldValue = SourceEvent.OldValue;
        Event.NewValue = SourceEvent.NewValue;
        Event.EffectiveValue = effectiveValue;
        Event.ValueType = RuntimeSettingValueTypeToString(Definition.ValueType);
        Event.Category = Definition.Category;
        Event.ApplyMode = Definition.ApplyMode;
        Event.bDeleted = bDeleted;
        Event.bValid = true;
        Event.bRestartRequired = Definition.ApplyMode == ERuntimeSettingApplyMode::RestartRequired;
        Event.UpdatedBy = SourceEvent.UpdatedBy;
        Event.UpdatedAtUnix = SourceEvent.UpdatedAtUnix > 0 ? SourceEvent.UpdatedAtUnix : static_cast<int64_t>(std::time(nullptr));
        Event.Message = validateMessage;
    }

    return DispatchEvent(Event);
}

FRuntimeSettingApplyResult RuntimeSettingManager::DispatchEvent(const FRuntimeSettingChangeEvent& Event)
{
    std::vector<IRuntimeSettingObserver*> observers;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        observers = m_Observers;
    }

    FRuntimeSettingApplyResult aggregate;
    aggregate.bOk = true;
    aggregate.bApplied = false;
    aggregate.bRestartRequired = Event.bRestartRequired;
    aggregate.ApplyMode = Event.ApplyMode;
    aggregate.EffectiveValue = Event.EffectiveValue;

    if (Event.ApplyMode == ERuntimeSettingApplyMode::RestartRequired) {
        aggregate.Message = "Saved. Restart YuanBook service to apply this setting.";
    } else if (Event.ApplyMode == ERuntimeSettingApplyMode::Unsupported) {
        aggregate.bOk = false;
        aggregate.Message = "Unsupported runtime setting.";
    } else {
        aggregate.Message = Event.ApplyMode == ERuntimeSettingApplyMode::SoftReload
            ? "Saved. Soft reload applied."
            : "Applied immediately.";
    }

    for (IRuntimeSettingObserver* observer : observers) {
        if (!observer || !observer->CanHandleSetting(Event.SettingKey)) continue;

        FRuntimeSettingApplyResult single = observer->OnRuntimeSettingChanged(Event, *this);
        aggregate.bOk = aggregate.bOk && single.bOk;
        aggregate.bApplied = aggregate.bApplied || single.bApplied;
        aggregate.bRestartRequired = aggregate.bRestartRequired || single.bRestartRequired;
        if (!single.Message.empty()) {
            aggregate.Message = single.Message;
        }
    }

    if (Event.ApplyMode == ERuntimeSettingApplyMode::Immediate && !aggregate.bApplied && aggregate.bOk) {
        aggregate.bApplied = true;
    }
    if (Event.ApplyMode == ERuntimeSettingApplyMode::SoftReload && !aggregate.bApplied && aggregate.bOk) {
        aggregate.bApplied = true;
    }
    if (Event.ApplyMode == ERuntimeSettingApplyMode::RestartRequired) {
        aggregate.bApplied = false;
        aggregate.bRestartRequired = true;
    }

    return aggregate;
}

bool RuntimeSettingManager::ValidateByDefinition(const FRuntimeSettingDefinition& Definition,
                                                 const std::string& RawValue,
                                                 std::string& OutMessage) const
{
    if (Definition.ApplyMode == ERuntimeSettingApplyMode::Unsupported) {
        OutMessage = "Unsupported system setting";
        return false;
    }

    if (Definition.ValueType == ERuntimeSettingValueType::String) {
        if ((Definition.Key == "server.listenIp" || Definition.Key == "server.wwwDir" ||
             Definition.Key == "voiceLedger.endpoint" || Definition.Key == "voiceLedger.model" ||
             Definition.Key == "voiceLedger.curlPath") && RawValue.empty()) {
            OutMessage = "Value cannot be empty";
            return false;
        }
        OutMessage = "OK";
        return true;
    }

    if (Definition.ValueType == ERuntimeSettingValueType::Int) {
        int value = 0;
        if (!TryParseInt(RawValue, value)) {
            OutMessage = "Value must be an integer";
            return false;
        }
        if (Definition.bHasIntRange && (value < Definition.MinInt || value > Definition.MaxInt)) {
            std::ostringstream oss;
            oss << "Value must be between " << Definition.MinInt << " and " << Definition.MaxInt;
            OutMessage = oss.str();
            return false;
        }
        OutMessage = "OK";
        return true;
    }

    if (Definition.ValueType == ERuntimeSettingValueType::Double) {
        double value = 0.0;
        if (!TryParseDouble(RawValue, value)) {
            OutMessage = "Value must be a number";
            return false;
        }
        if (Definition.bHasDoubleRange && (value < Definition.MinDouble || value > Definition.MaxDouble)) {
            std::ostringstream oss;
            oss << "Value must be between " << Definition.MinDouble << " and " << Definition.MaxDouble;
            OutMessage = oss.str();
            return false;
        }
        OutMessage = "OK";
        return true;
    }

    if (Definition.ValueType == ERuntimeSettingValueType::Bool) {
        bool value = false;
        if (!TryParseBool(RawValue, value)) {
            OutMessage = "Value must be a boolean";
            return false;
        }
        OutMessage = "OK";
        return true;
    }

    OutMessage = "OK";
    return true;
}
