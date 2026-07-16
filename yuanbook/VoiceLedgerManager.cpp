// VoiceLedgerManager.cpp — 语音记账管理器实现
//
// 核心流程:
//   1. HTTP 接口入队语音文本任务
//   2. 工作线程出队 → 构建 DeepSeek 请求 → curl 调用 HTTPS API → 解析 JSON → 写入数据库
//
// HTTPS 依赖: 系统 curl 命令（树莓派部署环境提供）
//

#include "VoiceLedgerManager.h"
#include "WorkLog.h"
#include "LedgerManager.h"
#include "JsonLite.h"
#include "SystemUtils.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <ctime>

#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

using namespace std;
namespace fs = std::filesystem;

// ============================================================================
// 语音记账管理器实现
// ============================================================================

// ============================================================================
// 构造 / 析构
// ============================================================================

VoiceLedgerManager::VoiceLedgerManager()
{
    srand((unsigned int)time(nullptr));
}

VoiceLedgerManager::~VoiceLedgerManager()
{
    Shutdown();
}

// ============================================================================
// 配置归一化与热更新
// ============================================================================

void VoiceLedgerManager::NormalizeConfig(FVoiceLedgerDeepSeekConfig& InOutConfig)
{
    if (InOutConfig.Endpoint.empty()) {
        InOutConfig.Endpoint = "https://api.deepseek.com/chat/completions";
    }
    if (InOutConfig.Model.empty()) {
        InOutConfig.Model = "deepseek-chat";
    }
    if (InOutConfig.CurlPath.empty()) {
        InOutConfig.CurlPath = "curl";
    }
    if (InOutConfig.TimeoutSec <= 0) {
        InOutConfig.TimeoutSec = 60;
    }
    if (InOutConfig.Temperature < 0.0) {
        InOutConfig.Temperature = 0.0;
    }
    if (InOutConfig.Temperature > 2.0) {
        InOutConfig.Temperature = 2.0;
    }
    if (InOutConfig.MaxTokens <= 0) {
        InOutConfig.MaxTokens = 512;
    }
}

bool VoiceLedgerManager::IsConfigUsable(const FVoiceLedgerDeepSeekConfig& ConfigSnapshot,
                                         bool bEnabled,
                                         string* OutError)
{
    if (!bEnabled) {
        if (OutError) *OutError = "Voice ledger feature is disabled";
        return false;
    }
    if (ConfigSnapshot.ApiKey.empty()) {
        if (OutError) *OutError = "DeepSeek apiKey is empty";
        return false;
    }
    if (ConfigSnapshot.Endpoint.empty()) {
        if (OutError) *OutError = "DeepSeek endpoint is empty";
        return false;
    }
    if (ConfigSnapshot.Model.empty()) {
        if (OutError) *OutError = "DeepSeek model is empty";
        return false;
    }
    if (ConfigSnapshot.CurlPath.empty()) {
        if (OutError) *OutError = "curl path is empty";
        return false;
    }
    if (ConfigSnapshot.TimeoutSec <= 0) {
        if (OutError) *OutError = "DeepSeek timeout must be positive";
        return false;
    }
    if (ConfigSnapshot.MaxTokens <= 0) {
        if (OutError) *OutError = "DeepSeek maxTokens must be positive";
        return false;
    }
    return true;
}

bool VoiceLedgerManager::UpdateConfig(const FVoiceLedgerDeepSeekConfig& Config, bool bEnabled)
{
    FVoiceLedgerDeepSeekConfig normalized = Config;
    NormalizeConfig(normalized);

    {
        lock_guard<mutex> lock(m_ConfigMutex);
        m_Config = normalized;
    }

    m_bEnabled.store(bEnabled);

    string unavailableReason;
    const bool bUsable = IsConfigUsable(normalized, bEnabled, &unavailableReason);
    m_bAvailable.store(bUsable);

    if (bUsable) {
        printf("[VoiceLedger] Runtime config applied: endpoint=%s, model=%s, curl=%s, timeout=%d, maxTokens=%d\n",
               normalized.Endpoint.c_str(),
               normalized.Model.c_str(),
               normalized.CurlPath.c_str(),
               normalized.TimeoutSec,
               normalized.MaxTokens);
    } else {
        fprintf(stderr, "[VoiceLedger] Runtime config applied but unavailable: %s\n",
                unavailableReason.c_str());
    }

    return true;
}

FVoiceLedgerDeepSeekConfig VoiceLedgerManager::GetConfigSnapshot() const
{
    lock_guard<mutex> lock(m_ConfigMutex);
    return m_Config;
}

// ============================================================================
// 初始化：保存 DeepSeek 配置 + 启动工作线程
// ============================================================================

bool VoiceLedgerManager::Initialize(const FVoiceLedgerDeepSeekConfig& Config,
                                    LedgerManager* pLedger)
{
    if (m_bRunning.load()) return false;

    m_pLedger = pLedger;
    if (!m_pLedger) {
        fprintf(stderr, "[VoiceLedger] ERROR: LedgerManager not set\n");
        m_bAvailable.store(false);
        return false;
    }

    // ---- 初始化阶段只建立运行态快照；API Key 可后续通过系统参数热更新补齐。 ----
    UpdateConfig(Config, true);

    // ---- 启动工作线程。即使当前配置不可用，也允许后续热更新恢复处理能力。 ----
    m_bRunning.store(true);
    m_WorkerThread = thread(&VoiceLedgerManager::WorkerLoop, this);

    return true;
}

// ============================================================================
// 关闭
// ============================================================================

void VoiceLedgerManager::Shutdown()
{
    if (!m_bRunning.load()) return;

    printf("[VoiceLedger] Shutting down...\n");

    m_bRunning.store(false);
    m_bAvailable.store(false);
    {
        lock_guard<mutex> lock(m_QueueMutex);
        m_TaskQueue.clear();
    }
    m_QueueCV.notify_all();

    if (m_WorkerThread.joinable()) {
        m_WorkerThread.join();
    }

    printf("[VoiceLedger] Shutdown complete.\n");
}

// ============================================================================
// 入队
// ============================================================================

void VoiceLedgerManager::Enqueue(const FVoiceLedgerTask& Task)
{
    {
        lock_guard<mutex> lock(m_QueueMutex);
        m_TaskQueue.push_back(Task);
    }
    m_QueueCV.notify_one();

    printf("[VoiceLedger] Task enqueued: \"%s\" (ledgerId=%d, user=%s)\n",
           Task.RawText.c_str(), Task.LedgerId, Task.Username.c_str());
}

// ============================================================================
// 工作线程主循环
// ============================================================================

void VoiceLedgerManager::WorkerLoop()
{
    printf("[VoiceLedger] Worker thread started\n");

    while (m_bRunning.load()) {
        vector<FVoiceLedgerTask> tasks;

        // ---- 1. 优先从数据库待解析队列轮询语音流水 ----
        if (m_pLedger) {
            vector<FLedgerVoicePendingItem> pendingItems;
            if (m_pLedger->GetPendingVoiceInputTransactions(pendingItems, 5)) {
                for (const auto& item : pendingItems) {
                    if (!m_pLedger->MarkVoiceInputProcessing(item.Id)) {
                        continue;
                    }

                    FVoiceLedgerTask task;
                    task.TransactionId = item.Id;
                    task.RawText       = item.VoiceText;
                    task.LedgerId      = item.LedgerId;
                    task.Username      = item.Username;
                    tasks.push_back(task);
                }
            }
        }

        // ---- 2. 兼容旧内存队列任务；不阻塞，后台仍每秒轮询数据库 ----
        {
            unique_lock<mutex> lock(m_QueueMutex);
            if (tasks.empty() && m_TaskQueue.empty()) {
                m_QueueCV.wait_for(lock, chrono::seconds(1), [this]() {
                    return !m_TaskQueue.empty() || !m_bRunning.load();
                });
            }

            while (!m_TaskQueue.empty()) {
                tasks.push_back(m_TaskQueue.front());
                m_TaskQueue.pop_front();
            }
        }

        if (!m_bRunning.load()) break;
        if (tasks.empty()) continue;

        for (const auto& task : tasks) {
            if (!m_bRunning.load()) break;

            const int userId = m_pLedger ? m_pLedger->GetUserId(task.Username) : 0;
            FScopedWorkLogUserContext logContext(userId > 0 ? userId : 0);

            printf("[VoiceLedger] Processing%s id=%d: \"%s\"\n",
                   task.TransactionId > 0 ? " DB voice transaction" : " legacy task",
                   task.TransactionId, task.RawText.c_str());
            auto startTime = chrono::steady_clock::now();

            bool bOk = ProcessTask(task);

            auto endTime = chrono::steady_clock::now();
            auto elapsedMs = chrono::duration_cast<chrono::milliseconds>(
                endTime - startTime).count();

            if (bOk) {
                printf("[VoiceLedger] Task completed in %lld ms\n",
                       (long long)elapsedMs);
            } else {
                fprintf(stderr, "[VoiceLedger] Task FAILED after %lld ms\n",
                        (long long)elapsedMs);
            }
        }
    }

    printf("[VoiceLedger] Worker thread stopped\n");
}

// ============================================================================
// 同步测试接口
// ============================================================================

bool VoiceLedgerManager::TestVoice(const FVoiceLedgerTask& Task,
                                   vector<FParsedEntry>& OutEntries)
{
    if (!m_bAvailable.load()) {
        fprintf(stderr, "[VoiceLedger] DeepSeek API is not available\n");
        return false;
    }

    const FVoiceLedgerDeepSeekConfig configSnapshot = GetConfigSnapshot();
    string unavailableReason;
    if (!IsConfigUsable(configSnapshot, m_bEnabled.load(), &unavailableReason)) {
        fprintf(stderr, "[VoiceLedger] TestVoice: %s\n", unavailableReason.c_str());
        return false;
    }

    string requestJson;
    if (!BuildDeepSeekRequest(Task.RawText,
                              Task.LedgerId,
                              Task.Username,
                              configSnapshot,
                              requestJson)) {
        fprintf(stderr, "[VoiceLedger] TestVoice: Failed to build request\n");
        return false;
    }

    printf("[VoiceLedger] TestVoice: DeepSeek request %zu chars\n", requestJson.size());

    string error;
    vector<FParsedEntry> entries;
    if (!RunDeepSeekRequest(requestJson, configSnapshot, entries, error)) {
        fprintf(stderr, "[VoiceLedger] TestVoice: %s\n", error.c_str());
        return false;
    }

    if (entries.empty()) {
        fprintf(stderr, "[VoiceLedger] TestVoice: No entries parsed\n");
        return false;
    }

    printf("[VoiceLedger] TestVoice: Parsed %zu entries (not saved to DB)\n",
           entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        printf("[VoiceLedger]   [%zu] %s | %.2f | %s | %s\n",
               i,
               entries[i].CategoryName.c_str(),
               entries[i].Amount,
               entries[i].Type.c_str(),
               entries[i].Description.c_str());
    }

    OutEntries = std::move(entries);
    return true;
}

// ============================================================================
// 批量分类（账单导入专用）
// ============================================================================

bool VoiceLedgerManager::BatchCategorize(
    const vector<string>& SourceTexts,
    int LedgerId,
    const string& RequestedByUsername,
    vector<string>& OutCategories,
    vector<string>& OutDescriptions)
{
    OutCategories.clear();
    OutDescriptions.clear();

    if (SourceTexts.empty()) return true;

    const FVoiceLedgerDeepSeekConfig configSnapshot = GetConfigSnapshot();
    string unavailableReason;
    if (!IsConfigUsable(configSnapshot, m_bEnabled.load(), &unavailableReason)) {
        fprintf(stderr, "[VoiceLedger] BatchCategorize: %s\n", unavailableReason.c_str());
        return false;
    }

    // ---- 1. 构建批量提示词 ----
    if (!m_pLedger) {
        fprintf(stderr, "[VoiceLedger] BatchCategorize: LedgerManager not set\n");
        return false;
    }

    // 分类上下文必须通过显式用户身份从目标账本加载，后台导入任务不得绕过家庭成员授权。
    string categoriesJson;
    string categoryError;
    if (!m_pLedger->GetLedgerCategories(LedgerId,
                                        "",
                                        RequestedByUsername,
                                        categoriesJson,
                                        categoryError)) {
        fprintf(stderr,
                "[VoiceLedger] BatchCategorize: Failed to query categories for ledger %d: %s\n",
                LedgerId,
                categoryError.c_str());
        return false;
    }

    // 提取分类名称列表
    vector<string> expenseCategories;
    vector<string> incomeCategories;
    size_t pos = 0;
    while (true) {
        pos = categoriesJson.find('{', pos);
        if (pos == string::npos) break;

        int depth = 0;
        size_t start = pos;
        for (; pos < categoriesJson.size(); ++pos) {
            if (categoriesJson[pos] == '{') depth++;
            else if (categoriesJson[pos] == '}') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (pos >= categoriesJson.size()) break;

        string obj = categoriesJson.substr(start, pos - start + 1);
        pos++;

        string name = JsonLite::GetStringOrDefault(obj, "name");
        string type = JsonLite::GetStringOrDefault(obj, "type");
        int parentId = (int)JsonLite::GetDoubleOrDefault(obj, "parentId", 0);

        if (name.empty()) continue;
        if (parentId != 0) continue;

        if (type == "income") {
            incomeCategories.push_back(name);
        } else {
            expenseCategories.push_back(name);
        }
    }

    // 构建分类列表字符串
    ostringstream catList;
    catList << "[";
    bool first = true;
    for (auto& c : expenseCategories) {
        if (!first) catList << ", ";
        first = false;
        catList << c;
    }
    for (auto& c : incomeCategories) {
        if (!first) catList << ", ";
        first = false;
        catList << c;
    }
    catList << "]";
    string catListStr = catList.str();

    // ---- 2. 构建 DeepSeek messages ----
    ostringstream systemPrompt;
    systemPrompt << "你是一个家庭记账助手。请为以下每条交易记录分配分类并给出简短描述。";
    systemPrompt << "每条记录输出一个 JSON 对象，包含 category 和 description 两个字段。";
    systemPrompt << "category 必须从用户提供的可用分类中精确选择一个。";
    systemPrompt << "description 是该交易的简短中文描述（如购买的商品或服务名称）。";
    systemPrompt << "如果无法确定分类，支出默认用\"其他支出\"，收入默认用\"其他收入\"。";
    systemPrompt << "请严格按照交易记录的编号顺序输出 JSON 数组。";
    systemPrompt << "只输出 JSON 数组，不要输出 Markdown、代码块或解释。";

    ostringstream userPrompt;
    userPrompt << "可用分类: " << catListStr << "\n\n";
    userPrompt << "交易记录列表:\n";

    for (size_t i = 0; i < SourceTexts.size(); ++i) {
        userPrompt << (i + 1) << ". " << SourceTexts[i] << "\n";
    }

    userPrompt << "\n请按顺序输出 JSON 数组，每个元素包含 category 和 description：\n";
    userPrompt << "示例输出: [{\"category\":\"餐饮\",\"description\":\"早餐\"},{\"category\":\"交通\",\"description\":\"加油\"}]";

    // ---- 3. 构建请求 JSON：固定使用进入本次调用时捕获的配置快照。 ----
    ostringstream req;
    req << "{";
    req << "\"model\":\"" << JsonLite::EscapeString(configSnapshot.Model) << "\",";
    req << "\"messages\":[";
    req << "{\"role\":\"system\",\"content\":\"" << JsonLite::EscapeString(systemPrompt.str()) << "\"},";
    req << "{\"role\":\"user\",\"content\":\"" << JsonLite::EscapeString(userPrompt.str()) << "\"}";
    req << "],";
    req << "\"temperature\":" << configSnapshot.Temperature << ",";
    req << "\"max_tokens\":" << (configSnapshot.MaxTokens * (int)SourceTexts.size()) << ",";
    req << "\"stream\":false";
    req << "}";

    string requestJson = req.str();
    printf("[VoiceLedger] BatchCategorize: %zu items, request %zu chars\n",
           SourceTexts.size(), requestJson.size());

    // ---- 4. 调用 DeepSeek API ----
    string error;
    vector<FParsedEntry> entries;
    if (!RunDeepSeekRequest(requestJson, configSnapshot, entries, error)) {
        fprintf(stderr, "[VoiceLedger] BatchCategorize: %s\n", error.c_str());
        return false;
    }

    // ---- 5. 按顺序映射结果 ----
    OutCategories.resize(SourceTexts.size());
    OutDescriptions.resize(SourceTexts.size());

    for (size_t i = 0; i < SourceTexts.size(); ++i) {
        // 默认值
        string defaultCat = "其他支出";
        // 尝试从第一行判断方向（支出还是收入）
        // 无法准确判断时默认支出
        OutCategories[i] = defaultCat;
        OutDescriptions[i] = "";
    }

    // 填充 DeepSeek 返回的结果
    for (size_t i = 0; i < entries.size() && i < SourceTexts.size(); ++i) {
        if (!entries[i].CategoryName.empty()) {
            OutCategories[i] = entries[i].CategoryName;
        }
        if (!entries[i].Description.empty()) {
            OutDescriptions[i] = entries[i].Description;
        }
    }

    printf("[VoiceLedger] BatchCategorize: %zu/%zu items categorized\n",
           entries.size(), SourceTexts.size());
    return true;
}

// ============================================================================
// 处理单个任务：请求构建 → DeepSeek API → 解析 → 存储
// ============================================================================

bool VoiceLedgerManager::ProcessTask(const FVoiceLedgerTask& Task)
{
    if (!m_bAvailable.load()) {
        string error = "DeepSeek API is not available";
        fprintf(stderr, "[VoiceLedger] %s\n", error.c_str());
        if (Task.TransactionId > 0 && m_pLedger) {
            m_pLedger->MarkVoiceInputFailed(Task.TransactionId, error);
        }
        return false;
    }

    string error;
    const FVoiceLedgerDeepSeekConfig configSnapshot = GetConfigSnapshot();
    if (!IsConfigUsable(configSnapshot, m_bEnabled.load(), &error)) {
        fprintf(stderr, "[VoiceLedger] %s\n", error.c_str());
        if (Task.TransactionId > 0 && m_pLedger) {
            m_pLedger->MarkVoiceInputFailed(Task.TransactionId, error);
        }
        return false;
    }

    // ---- 1. 构建 DeepSeek 请求；分类上下文严格限定在任务目标账本。 ----
    string requestJson;
    if (!BuildDeepSeekRequest(Task.RawText,
                              Task.LedgerId,
                              Task.Username,
                              configSnapshot,
                              requestJson)) {
        error = "Failed to build DeepSeek request";
        fprintf(stderr, "[VoiceLedger] %s\n", error.c_str());
        if (Task.TransactionId > 0 && m_pLedger) {
            m_pLedger->MarkVoiceInputFailed(Task.TransactionId, error);
        }
        return false;
    }

    printf("[VoiceLedger] DeepSeek request: %zu chars\n", requestJson.size());

    // ---- 2. DeepSeek API 解析 ----
    vector<FParsedEntry> entries;
    if (!RunDeepSeekRequest(requestJson, configSnapshot, entries, error)) {
        if (error == "Failed to parse DeepSeek model output") {
            entries.push_back(BuildFallbackParsedEntry(Task.RawText));
            printf("[VoiceLedger] Model output is not parseable JSON; using fallback voice entry\n");
        } else {
            if (error.empty()) error = "DeepSeek API request failed";
            fprintf(stderr, "[VoiceLedger] %s\n", error.c_str());
            if (Task.TransactionId > 0 && m_pLedger) {
                m_pLedger->MarkVoiceInputFailed(Task.TransactionId, error);
            }
            return false;
        }
    }

    if (entries.empty()) {
        error = "No entries parsed from DeepSeek output";
        fprintf(stderr, "[VoiceLedger] %s\n", error.c_str());
        if (Task.TransactionId > 0 && m_pLedger) {
            m_pLedger->MarkVoiceInputFailed(Task.TransactionId, error);
        }
        return false;
    }

    printf("[VoiceLedger] Parsed %zu entries:\n", entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        printf("[VoiceLedger]   [%zu] %s | %.2f | %s | completed=%d | %s\n",
               i,
               entries[i].CategoryName.c_str(),
               entries[i].Amount,
               entries[i].Type.c_str(),
               entries[i].ParseCompleted ? 1 : 0,
               entries[i].Description.c_str());
    }

    // ---- 3. DB 队列任务：回填当前流水；多段结果由账本层插入分段流水 ----
    if (Task.TransactionId > 0) {
        if (!m_pLedger) {
            fprintf(stderr, "[VoiceLedger] LedgerManager not set\n");
            return false;
        }

        vector<FLedgerParsedVoiceEntry> ledgerEntries;
        ledgerEntries.reserve(entries.size());
        for (const auto& entry : entries) {
            FLedgerParsedVoiceEntry ledgerEntry;
            ledgerEntry.CategoryName    = entry.CategoryName;
            ledgerEntry.Amount          = entry.Amount;
            ledgerEntry.Type            = entry.Type;
            ledgerEntry.Description     = entry.Description;
            ledgerEntry.ParseCompleted  = entry.ParseCompleted;
            ledgerEntries.push_back(ledgerEntry);
        }

        if (!m_pLedger->ApplyVoiceParseResult(Task.TransactionId, ledgerEntries, error)) {
            if (error.empty()) error = "Failed to apply voice parse result";
            fprintf(stderr, "[VoiceLedger] %s\n", error.c_str());
            m_pLedger->MarkVoiceInputFailed(Task.TransactionId, error);
            return false;
        }
        return true;
    }

    // ---- 4. 内存队列任务：按账本边界和显式用户身份新增流水。 ----
    return SaveEntries(entries, Task.LedgerId, Task.Username);
}

// ============================================================================
// 构建 DeepSeek Chat Completions 请求 JSON
// ============================================================================

bool VoiceLedgerManager::BuildDeepSeekRequest(
    const string& UserText,
    int LedgerId,
    const string& RequestedByUsername,
    const FVoiceLedgerDeepSeekConfig& ConfigSnapshot,
    string& OutRequestJson)
{
    if (!m_pLedger) return false;

    // ---- 通过显式请求用户加载目标账本分类，确保提示词上下文不会跨账本泄漏。 ----
    string categoriesJson;
    string categoryError;
    if (!m_pLedger->GetLedgerCategories(LedgerId,
                                        "",
                                        RequestedByUsername,
                                        categoriesJson,
                                        categoryError)) {
        fprintf(stderr,
                "[VoiceLedger] Failed to query categories for ledger %d: %s\n",
                LedgerId,
                categoryError.c_str());
        return false;
    }

    // 从 JSON 提取分类名称列表
    vector<string> expenseCategories;
    vector<string> incomeCategories;

    size_t pos = 0;
    while (true) {
        pos = categoriesJson.find('{', pos);
        if (pos == string::npos) break;

        int depth = 0;
        size_t start = pos;
        for (; pos < categoriesJson.size(); ++pos) {
            if (categoriesJson[pos] == '{') depth++;
            else if (categoriesJson[pos] == '}') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (pos >= categoriesJson.size()) break;

        string obj = categoriesJson.substr(start, pos - start + 1);
        pos++;

        string name = JsonLite::GetStringOrDefault(obj, "name");
        string type = JsonLite::GetStringOrDefault(obj, "type");
        int parentId = (int)JsonLite::GetDoubleOrDefault(obj, "parentId", 0);

        if (name.empty()) continue;
        // 只取一级分类（parentId == 0）
        if (parentId != 0) continue;

        if (type == "income") {
            incomeCategories.push_back(name);
        } else {
            expenseCategories.push_back(name);
        }
    }

    if (expenseCategories.empty() && incomeCategories.empty()) {
        fprintf(stderr, "[VoiceLedger] No categories found for ledger %d\n",
                LedgerId);
        return false;
    }

    // ---- 构建分类列表字符串 ----
    ostringstream catList;
    catList << "[";
    bool first = true;
    for (auto& c : expenseCategories) {
        if (!first) catList << ", ";
        first = false;
        catList << c;
    }
    for (auto& c : incomeCategories) {
        if (!first) catList << ", ";
        first = false;
        catList << c;
    }
    catList << "]";
    string catListStr = catList.str();

    // ---- 构建 DeepSeek messages ----
    ostringstream systemPrompt;
    systemPrompt << "你是一个家庭记账助手。请把用户的中文口语消费或收入描述解析为 JSON 数组。";
    systemPrompt << "只输出 JSON 数组，不要输出 Markdown、代码块或解释。";
    systemPrompt << "数组中每个对象必须包含 category、amount、type、description、parseCompleted 五个字段。";
    systemPrompt << "category 必须从用户提供的可用分类中精确选择一个。";
    systemPrompt << "amount 是以元为单位的非负数；如果输入没有金额或金额无法识别，amount 必须填 0。";
    systemPrompt << "parseCompleted 是布尔值；只要已从输入中解析出一条应保留的账目记录，即使金额缺失也必须为 true。";
    systemPrompt << "type 只能是 expense 或 income。";
    systemPrompt << "description 是简短中文描述。";
    systemPrompt << "语音识别可能有同音字错误，请根据语义纠正。";

    ostringstream userPrompt;
    userPrompt << "可用分类: " << catListStr << "\n\n";
    userPrompt << "示例输入: 早餐 7 元，停车费 50\n";
    userPrompt << "示例输出: [{\"category\":\"餐饮\",\"amount\":7,\"type\":\"expense\",\"description\":\"早餐\",\"parseCompleted\":true},{\"category\":\"交通\",\"amount\":50,\"type\":\"expense\",\"description\":\"停车费\",\"parseCompleted\":true}]\n\n";
    userPrompt << "示例输入: 今天早上吃了个饭花了欺块钱\n";
    userPrompt << "示例输出: [{\"category\":\"餐饮\",\"amount\":7,\"type\":\"expense\",\"description\":\"早餐\",\"parseCompleted\":true}]\n\n";
    userPrompt << "示例输入: 买了一个东西\n";
    userPrompt << "示例输出: [{\"category\":\"其他支出\",\"amount\":0,\"type\":\"expense\",\"description\":\"买了一个东西\",\"parseCompleted\":true}]\n\n";
    userPrompt << "现在解析以下输入，只输出 JSON 数组:\n" << UserText;

    ostringstream req;
    req << "{";
    req << "\"model\":\"" << JsonLite::EscapeString(ConfigSnapshot.Model) << "\",";
    req << "\"messages\":[";
    req << "{\"role\":\"system\",\"content\":\"" << JsonLite::EscapeString(systemPrompt.str()) << "\"},";
    req << "{\"role\":\"user\",\"content\":\"" << JsonLite::EscapeString(userPrompt.str()) << "\"}";
    req << "],";
    req << "\"temperature\":" << ConfigSnapshot.Temperature << ",";
    req << "\"max_tokens\":" << ConfigSnapshot.MaxTokens << ",";
    req << "\"stream\":false";
    req << "}";

    OutRequestJson = req.str();
    return true;
}

// ============================================================================
// curl 调用 DeepSeek API
// ============================================================================

bool VoiceLedgerManager::RunDeepSeekRequest(
    const string& RequestJson,
    const FVoiceLedgerDeepSeekConfig& ConfigSnapshot,
    vector<FParsedEntry>& OutEntries,
    string& OutError)
{
    OutEntries.clear();
    OutError.clear();

    string reqPath = SystemUtils::MakeTempFilePath("voice_deepseek_req", ".json");
    if (!SystemUtils::WriteTextFile(reqPath, RequestJson)) {
        OutError = "Failed to write DeepSeek request temp file";
        return false;
    }

    string authHeader = "Authorization: Bearer " + ConfigSnapshot.ApiKey;
    const string resolvedCurlPath = SystemUtils::ResolveExecutablePath(ConfigSnapshot.CurlPath);

    ostringstream cmd;
    cmd << SystemUtils::ShellQuote(resolvedCurlPath)
        << " -sS --fail-with-body"
        << " --connect-timeout " << ConfigSnapshot.TimeoutSec
        << " --max-time " << ConfigSnapshot.TimeoutSec
        << " -X POST"
        << " -H " << SystemUtils::ShellQuote("Content-Type: application/json")
        << " -H " << SystemUtils::ShellQuote(authHeader)
        << " --data-binary " << SystemUtils::ShellQuote("@" + reqPath)
        << " " << SystemUtils::ShellQuote(ConfigSnapshot.Endpoint);
#if !defined(_WIN32) && !defined(__CYGWIN__)
    // Unix-like 平台的 popen 仅捕获标准输出，因此由 shell 合并标准错误。
    cmd << " 2>&1";
#endif

    string response;
    string processStartError;
    int exitCode = -1;
    bool pipeOk = SystemUtils::ReadPipeAll(cmd.str(), response, exitCode, &processStartError);

    std::error_code ec;
    fs::remove(reqPath, ec);

    if (!pipeOk) {
        OutError = "Failed to start curl process: configured=" +
                   SystemUtils::ShellQuote(ConfigSnapshot.CurlPath) +
                   ", resolved=" + SystemUtils::ShellQuote(resolvedCurlPath);
        if (!processStartError.empty()) {
            OutError += ", detail=" + processStartError;
        }
        return false;
    }
    if (exitCode != 0) {
        OutError = "curl failed (exit=" + to_string(exitCode) + "): " + response;
        return false;
    }

    string content;
    if (!ExtractDeepSeekContent(response, content, OutError)) {
        if (OutError.empty()) OutError = "Failed to extract DeepSeek content";
        return false;
    }

#ifdef _DEBUG
    printf("[VoiceLedger] DeepSeek content:\n---\n%s\n---\n", content.c_str());
#endif

    if (!ParseModelOutput(content, OutEntries)) {
        OutError = "Failed to parse DeepSeek model output";
        return false;
    }

    return true;
}

bool VoiceLedgerManager::ExtractDeepSeekContent(
    const string& ResponseJson,
    string& OutContent,
    string& OutError)
{
    OutContent.clear();
    OutError.clear();

    if (ResponseJson.empty()) {
        OutError = "Empty DeepSeek response";
        return false;
    }

    string apiError = JsonLite::GetStringOrDefault(ResponseJson, "message");
    if (ResponseJson.find("\"error\"") != string::npos && !apiError.empty()) {
        OutError = "DeepSeek API error: " + apiError;
        return false;
    }

    OutContent = JsonLite::GetStringOrDefault(ResponseJson, "content");
    if (OutContent.empty()) {
        OutError = "DeepSeek response missing choices[0].message.content";
        return false;
    }

    return true;
}

// ============================================================================
// 解析模型输出的 JSON 数组
// ============================================================================

bool VoiceLedgerManager::ParseModelOutput(
    const string& JsonOutput,
    vector<FParsedEntry>& OutEntries)
{
    OutEntries.clear();

    if (JsonOutput.empty()) return false;

    // 找到 JSON 数组 [...]；部分模型在无金额场景可能直接输出单个对象 {...}，这里兼容包装为数组。
    size_t arrStart = JsonOutput.find('[');
    size_t arrEnd = string::npos;
    string arrayStr;
    if (arrStart == string::npos) {
        size_t objStart = JsonOutput.find('{');
        if (objStart == string::npos) {
            fprintf(stderr, "[VoiceLedger] No JSON array or object found in output\n");
            return false;
        }

        int objDepth = 0;
        bool objInString = false;
        bool objEscaped = false;
        size_t objEnd = objStart;
        for (; objEnd < JsonOutput.size(); ++objEnd) {
            char ch = JsonOutput[objEnd];
            if (objInString) {
                if (objEscaped) {
                    objEscaped = false;
                } else if (ch == '\\') {
                    objEscaped = true;
                } else if (ch == '"') {
                    objInString = false;
                }
                continue;
            }
            if (ch == '"') objInString = true;
            else if (ch == '{') objDepth++;
            else if (ch == '}') {
                objDepth--;
                if (objDepth == 0) break;
            }
        }
        if (objEnd >= JsonOutput.size()) {
            fprintf(stderr, "[VoiceLedger] Unclosed JSON object in output\n");
            return false;
        }
        arrayStr = "[" + JsonOutput.substr(objStart, objEnd - objStart + 1) + "]";
    }

    if (arrayStr.empty()) {
    // 找到匹配的 ]，注意跳过 JSON 字符串内的括号
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    arrEnd = arrStart;
    for (; arrEnd < JsonOutput.size(); ++arrEnd) {
        char ch = JsonOutput[arrEnd];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') inString = true;
        else if (ch == '[') depth++;
        else if (ch == ']') {
            depth--;
            if (depth == 0) break;
        }
    }

    if (arrEnd >= JsonOutput.size()) {
        fprintf(stderr, "[VoiceLedger] Unclosed JSON array in output\n");
        return false;
    }

    arrayStr = JsonOutput.substr(arrStart, arrEnd - arrStart + 1);
    }

    // 解析每个 JSON 对象
    size_t pos = 0;
    while (pos < arrayStr.size()) {
        // 跳过空白和逗号
        while (pos < arrayStr.size() &&
               (arrayStr[pos] == ' ' || arrayStr[pos] == '\t' ||
                arrayStr[pos] == '\n' || arrayStr[pos] == '\r' ||
                arrayStr[pos] == ',' || arrayStr[pos] == '[' || arrayStr[pos] == ']')) {
            pos++;
        }
        if (pos >= arrayStr.size()) break;

        if (arrayStr[pos] != '{') { pos++; continue; }

        // 找匹配的 }，注意跳过 JSON 字符串内的括号
        int objDepth = 0;
        bool objInString = false;
        bool objEscaped = false;
        size_t objStart = pos;
        for (; pos < arrayStr.size(); ++pos) {
            char ch = arrayStr[pos];
            if (objInString) {
                if (objEscaped) {
                    objEscaped = false;
                } else if (ch == '\\') {
                    objEscaped = true;
                } else if (ch == '"') {
                    objInString = false;
                }
                continue;
            }

            if (ch == '"') objInString = true;
            else if (ch == '{') objDepth++;
            else if (ch == '}') {
                objDepth--;
                if (objDepth == 0) { pos++; break; }
            }
        }

        string objStr = arrayStr.substr(objStart, pos - objStart);

        FParsedEntry entry;
        entry.CategoryName    = JsonLite::GetStringOrDefault(objStr, "category");
        if (entry.CategoryName.empty()) entry.CategoryName = JsonLite::GetStringOrDefault(objStr, "categoryName");
        if (entry.CategoryName.empty()) entry.CategoryName = JsonLite::GetStringOrDefault(objStr, "分类");
        entry.Amount          = JsonLite::GetDoubleOrDefault(objStr, "amount", 0.0);
        entry.Type            = JsonLite::GetStringOrDefault(objStr, "type");
        if (entry.Type.empty()) entry.Type = JsonLite::GetStringOrDefault(objStr, "收支");
        entry.Description     = JsonLite::GetStringOrDefault(objStr, "description");
        if (entry.Description.empty()) entry.Description = JsonLite::GetStringOrDefault(objStr, "备注");
        if (entry.Description.empty()) entry.Description = JsonLite::GetStringOrDefault(objStr, "描述");
        entry.ParseCompleted  = JsonLite::GetBoolOrDefault(objStr, "parseCompleted", true);
        entry.ParseCompleted  = JsonLite::GetBoolOrDefault(objStr, "parsed", entry.ParseCompleted);
        entry.ParseCompleted  = JsonLite::GetBoolOrDefault(objStr, "completed", entry.ParseCompleted);

        // 容错：如果 type 为空，默认 expense；如果分类为空，按类型兜底到系统“其他”分类。
        if (entry.Type == "收入") entry.Type = "income";
        if (entry.Type == "支出") entry.Type = "expense";
        if (entry.Type.empty()) entry.Type = "expense";
        if (entry.Amount < 0.0) entry.Amount = 0.0;
        if (entry.CategoryName.empty()) entry.CategoryName = (entry.Type == "income" ? "其他收入" : "其他支出");
        if (entry.Description.empty()) entry.Description = "未填写金额的语音记录";

        if (entry.IsValid()) {
            OutEntries.push_back(entry);
        } else {
            fprintf(stderr, "[VoiceLedger] Skipping invalid entry: cat=%s, "
                    "amount=%.2f, type=%s, completed=%d\n",
                    entry.CategoryName.c_str(), entry.Amount,
                    entry.Type.c_str(), entry.ParseCompleted ? 1 : 0);
        }
    }

    return !OutEntries.empty();
}

FParsedEntry VoiceLedgerManager::BuildFallbackParsedEntry(const string& RawText) const
{
    FParsedEntry entry;
    entry.CategoryName = "其他支出";
    entry.Amount = 0.0;
    entry.Type = "expense";
    entry.Description = SystemUtils::Trim(RawText);
    if (entry.Description.empty()) {
        entry.Description = "未填写金额的语音记录";
    }
    entry.ParseCompleted = true;
    return entry;
}

// ============================================================================
// 将解析结果写入数据库
// ============================================================================

bool VoiceLedgerManager::SaveEntries(
    const vector<FParsedEntry>& Entries,
    int LedgerId,
    const string& Username)
{
    if (!m_pLedger) {
        fprintf(stderr, "[VoiceLedger] LedgerManager not set\n");
        return false;
    }

    // 获取今天的日期 YYYY-MM-DD
    time_t now = time(nullptr);
    struct tm* tmLocal = localtime(&now);
    char dateBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
             tmLocal->tm_year + 1900,
             tmLocal->tm_mon + 1,
             tmLocal->tm_mday);
    string today(dateBuf);

    int savedCount = 0;
    for (const auto& entry : Entries) {
        int categoryId = 0;

        // 通过显式用户身份读取目标账本分类；分类名称相同也不得跨账本复用 ID。
        string catsJson;
        string categoryError;
        if (m_pLedger->GetLedgerCategories(LedgerId,
                                           entry.Type,
                                           Username,
                                           catsJson,
                                           categoryError)) {
            size_t pos = 0;
            while (pos < catsJson.size()) {
                pos = catsJson.find('{', pos);
                if (pos == string::npos) break;

                int depth = 0;
                size_t start = pos;
                for (; pos < catsJson.size(); ++pos) {
                    if (catsJson[pos] == '{') depth++;
                    else if (catsJson[pos] == '}') {
                        depth--;
                        if (depth == 0) break;
                    }
                }
                if (pos >= catsJson.size()) break;

                string obj = catsJson.substr(start, pos - start + 1);
                pos++;

                const string name = JsonLite::GetStringOrDefault(obj, "name");
                if (name == entry.CategoryName) {
                    categoryId = (int)JsonLite::GetDoubleOrDefault(obj, "id", 0);
                    break;
                }
            }
        } else {
            fprintf(stderr,
                    "[VoiceLedger] Failed to query ledger %d categories for user %s: %s\n",
                    LedgerId,
                    Username.c_str(),
                    categoryError.c_str());
        }

        if (categoryId <= 0) {
            fprintf(stderr, "[VoiceLedger] Category '%s' not found in ledger %d, "
                    "skipping entry\n",
                    entry.CategoryName.c_str(), LedgerId);
            continue;
        }

        int outId = 0;
        string saveError;
        if (m_pLedger->CreateLedgerTransaction(
                LedgerId,
                categoryId,
                entry.Amount,
                entry.Type,
                entry.Description,
                Username,
                today,
                outId,
                saveError)) {
            savedCount++;
            printf("[VoiceLedger] Saved: id=%d, ledgerId=%d, cat=%s, amount=%.2f, type=%s\n",
                   outId, LedgerId, entry.CategoryName.c_str(),
                   entry.Amount, entry.Type.c_str());
        } else {
            fprintf(stderr, "[VoiceLedger] Failed to save transaction: "
                    "ledgerId=%d, cat=%s, amount=%.2f, error=%s\n",
                    LedgerId,
                    entry.CategoryName.c_str(),
                    entry.Amount,
                    saveError.c_str());
        }
    }

    printf("[VoiceLedger] Saved %d/%zu transactions\n",
           savedCount, Entries.size());
    return savedCount > 0;
}

