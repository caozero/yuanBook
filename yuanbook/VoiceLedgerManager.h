// VoiceLedgerManager.h — 语音记账管理器
// 接收语音识别自然文本，通过 DeepSeek API 解析为结构化记账数据
//
// LLM: DeepSeek OpenAI-compatible Chat Completions API
// HTTPS: 通过系统 curl 命令访问，避免交叉编译时链接 OpenSSL
// 异步: 内存队列 + 独立工作线程，POST 接口即时返回
//

#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>

// ---------------------------------------------------------------------------
// 前向声明
// ---------------------------------------------------------------------------
class LedgerManager;

// ============================================================================
// DeepSeek API 配置
// ============================================================================
/**
 * @brief 描述 DeepSeek Chat Completions 接口的运行时配置。
 *
 * 该结构聚合了语音记账与批量账单分类共用的外部调用参数，
 * 便于从配置文件统一注入并在启动阶段完成有效性校验。
 */
struct FVoiceLedgerDeepSeekConfig
{
    /** OpenAI-compatible Chat Completions HTTP 接口地址。 */
    std::string Endpoint = "https://api.deepseek.com/chat/completions";

    /** 要调用的模型名称，例如 `deepseek-chat`。 */
    std::string Model = "deepseek-chat";

    /** 用于鉴权的 API Key；为空时表示功能不可用。 */
    std::string ApiKey;

    /** 系统 `curl` 可执行文件路径或命令名。 */
    std::string CurlPath = "curl";

    /** 单次 HTTP 请求超时时间，单位为秒。 */
    int TimeoutSec = 60;

    /** 传给模型的 temperature 参数，越低越稳定。 */
    double Temperature = 0.1;

    /** 单次请求允许生成的最大 token 数。 */
    int MaxTokens = 512;
};

// ============================================================================
// 语音记账任务
// ============================================================================
/**
 * @brief 表示一条待执行的语音记账任务。
 *
 * 任务既可以来自数据库中的待处理流水，也可以来自内存队列；所有分类查询和流水落库均以
 * [`FVoiceLedgerTask::LedgerId`](yuanbook/VoiceLedgerManager.h:76) 为唯一业务边界，禁止回退到家庭组作用域。
 */
struct FVoiceLedgerTask
{
    /** 待解析语音流水 ID；为 `0` 时表示由内存队列直接创建新流水。 */
    int TransactionId = 0;

    /** 原始自然语言文本，例如“早餐 7 元，停车费 50”。 */
    std::string RawText;

    /** 目标账本 ID，用于限定分类查询、授权校验与流水落库范围。 */
    int LedgerId = 0;

    /** 发起任务的用户名，用于家庭成员授权、审计与流水创建者记录。 */
    std::string Username;
};

// ============================================================================
// 解析后的单条记账条目
// ============================================================================
/**
 * @brief 表示模型解析后得到的一条结构化记账结果。
 *
 * 该结构是语音文本与数据库流水之间的中间层，
 * 既可用于测试接口返回，也可用于最终保存到数据库。
 */
struct FParsedEntry
{
    /** 分类名称，例如“餐饮”“交通”或“其他收入”。 */
    std::string CategoryName;

    /** 金额，约定为非负数；输入未包含金额时为 `0`。 */
    double Amount = 0.0;

    /** 流水方向，只允许 `expense` 或 `income`。 */
    std::string Type;

    /** 简短描述，例如“早餐”“停车费”。 */
    std::string Description;

    /** 是否已完成结构化解析；金额缺失但文本可解析时仍为 true。 */
    bool ParseCompleted = false;

    /**
     * @brief 判断当前条目是否满足最基本的业务合法性要求。
     * @return true 表示分类、类型有效，且解析已完成；完成解析允许金额为 `0`。
     */
    bool IsValid() const {
        return ParseCompleted && Amount >= 0.0 && !CategoryName.empty() &&
               (Type == "expense" || Type == "income");
    }
};

// ============================================================================
// VoiceLedgerManager — 语音记账管理器
// ============================================================================
class VoiceLedgerManager
{
public:
    /** 构造管理器对象，不立即启动后台线程。 */
    VoiceLedgerManager();

    /** 析构时会调用 [`VoiceLedgerManager::Shutdown()`](../VoiceLedgerManager.h:108) 回收后台线程。 */
    ~VoiceLedgerManager();

    // ---- 生命周期 ----

    /**
     * @brief 注入 DeepSeek 配置与账本依赖，并启动后台工作线程。
     *
     * @param Config DeepSeek HTTP 调用配置。
     * @param pLedger 非拥有型账本管理器指针，可为空；为空时语音落库能力不可用。
     * @return true 表示初始化成功；false 表示配置无效或当前已初始化。
     */
    bool Initialize(const FVoiceLedgerDeepSeekConfig& Config, LedgerManager* pLedger);

    /**
     * @brief 停止后台线程并释放运行态资源。
     *
     * 该方法可安全重复调用；若线程未运行则直接返回。
     */
    void Shutdown();

    /**
     * @brief 判断当前 DeepSeek 配置是否可用于实际请求。
     * @return true 表示功能开关已开启、API 关键配置完整且账本依赖可用。
     */
    bool IsAvailable() const { return m_bAvailable.load(); }

    /**
     * @brief 热更新 DeepSeek 运行配置。
     *
     * 该接口只替换后续请求使用的配置快照，不会中断正在执行的 curl 子进程。
     * 已进入处理流程的请求会继续使用进入流程时捕获的配置副本，从而避免请求体、鉴权头和 endpoint
     * 在一次调用中混用不同版本的参数。
     *
     * @param Config 新的 DeepSeek HTTP 调用配置；空字段会按内置默认值归一化。
     * @param bEnabled 语音记账功能开关；false 时后续任务会立即判定为不可用。
     * @return true 表示配置已写入运行时快照；返回 true 不代表 API Key 一定非空。
     */
    bool UpdateConfig(const FVoiceLedgerDeepSeekConfig& Config, bool bEnabled);

    /**
     * @brief 获取当前生效配置的线程安全快照。
     * @return 当前 DeepSeek 配置副本；调用方可在无锁状态下长期持有。
     */
    FVoiceLedgerDeepSeekConfig GetConfigSnapshot() const;

    // ---- 任务入队 ----

    /**
     * @brief 将一条语音记账任务压入异步队列并立即返回。
     *
     * 调用方通常应先检查 [`VoiceLedgerManager::IsAvailable()`](../VoiceLedgerManager.h:121)。
     */
    void Enqueue(const FVoiceLedgerTask& Task);

    // ---- 同步测试接口 ----

    /**
     * @brief 同步调用模型解析语音文本，但不执行数据库写入。
     *
     * 主要用于管理后台测试模型连接、提示词效果和解析质量。
     *
     * @param Task 待测试的语音任务。
     * @param OutEntries 成功时返回解析出的结构化条目。
     * @return true 表示模型调用和输出解析都成功。
     */
    bool TestVoice(const FVoiceLedgerTask& Task,
                   std::vector<FParsedEntry>& OutEntries);

    // ---- 批量分类（账单导入专用） ----

    /**
     * @brief 将多条账单摘要合并为一次模型请求，批量返回分类和描述。
     *
     * 该接口专门服务于账单导入流程，目的是降低逐条请求的成本与延迟。分类集合只允许从目标账本加载，
     * 并通过发起导入的用户名执行家庭成员授权，避免后台任务绕过账本访问控制。
     *
     * @param SourceTexts 多条交易摘要文本，通常来自账单行的 `SourceText`。
     * @param LedgerId 目标账本 ID，用于加载该账本下可用分类集合。
     * @param RequestedByUsername 发起导入的用户名，必须是账本所属家庭成员。
     * @param OutCategories 成功时返回与输入顺序一一对应的分类名称列表。
     * @param OutDescriptions 成功时返回与输入顺序一一对应的描述列表。
     * @return true 表示授权、分类查询、模型请求和结果校验均成功。
     */
    bool BatchCategorize(const std::vector<std::string>& SourceTexts,
                         int LedgerId,
                         const std::string& RequestedByUsername,
                         std::vector<std::string>& OutCategories,
                         std::vector<std::string>& OutDescriptions);

private:
    // ---- 异步队列 ----
    /** 等待后台线程消费的语音任务队列。 */
    std::deque<FVoiceLedgerTask> m_TaskQueue;

    /** 保护 [`VoiceLedgerManager::m_TaskQueue`](../VoiceLedgerManager.h:162) 的互斥锁。 */
    std::mutex m_QueueMutex;

    /** 用于唤醒后台线程处理新任务的条件变量。 */
    std::condition_variable m_QueueCV;

    /** 异步语音记账工作线程对象。 */
    std::thread m_WorkerThread;

    /** 工作线程运行标志。 */
    std::atomic<bool> m_bRunning{false};

    // ---- DeepSeek API 状态 ----
    /** 当前配置是否满足可发起模型请求的最小条件。 */
    std::atomic<bool> m_bAvailable{false};

    /** 语音记账功能运行时开关；关闭时保留后台线程但拒绝新模型请求。 */
    std::atomic<bool> m_bEnabled{true};

    /** 保护 [`VoiceLedgerManager::m_Config`](../VoiceLedgerManager.h:239) 的互斥锁。 */
    mutable std::mutex m_ConfigMutex;

    /** 当前生效的 DeepSeek 运行配置副本，仅允许在持有 m_ConfigMutex 时直接访问。 */
    FVoiceLedgerDeepSeekConfig m_Config;

    // ---- 依赖 ----
    /** 非拥有型账本管理器指针，用于读取分类与保存解析结果。 */
    LedgerManager* m_pLedger = nullptr;

    // ---- 工作线程 ----
    /** 后台线程入口，负责持续消费 [`FVoiceLedgerTask`](../VoiceLedgerManager.h:47)。 */
    void WorkerLoop();

    // ---- 任务处理 ----
    /**
     * @brief 处理单条语音记账任务。
     * @param Task 待处理任务。
     * @return true 表示模型解析和后续保存流程成功。
     */
    bool ProcessTask(const FVoiceLedgerTask& Task);

    /**
     * @brief 构建发送给 DeepSeek Chat Completions 的请求 JSON。
     * @param UserText 用户原始语音文本。
     * @param LedgerId 目标账本 ID，用于生成账本级分类上下文。
     * @param RequestedByUsername 请求用户名，必须具备目标账本访问权。
     * @param ConfigSnapshot 本次请求固定使用的配置快照。
     * @param OutRequestJson 成功时返回完整 JSON 请求体。
     * @return true 表示账本分类加载和请求构建均成功。
     */
    bool BuildDeepSeekRequest(const std::string& UserText,
                              int LedgerId,
                              const std::string& RequestedByUsername,
                              const FVoiceLedgerDeepSeekConfig& ConfigSnapshot,
                              std::string& OutRequestJson);

    /**
     * @brief 通过系统 `curl` 调用 DeepSeek 接口并解析为条目列表。
     * @param RequestJson 已构造好的请求 JSON。
     * @param ConfigSnapshot 本次请求固定使用的配置快照。
     * @param OutEntries 成功时返回解析出的条目列表。
     * @param OutError 失败时返回错误描述。
     */
    bool RunDeepSeekRequest(const std::string& RequestJson,
                            const FVoiceLedgerDeepSeekConfig& ConfigSnapshot,
                            std::vector<FParsedEntry>& OutEntries,
                            std::string& OutError);

    /**
     * @brief 归一化配置字段，确保运行态不会出现空 endpoint/model/curlPath 或非法数值。
     * @param InOutConfig 待修正配置；函数会就地写回默认值。
     */
    static void NormalizeConfig(FVoiceLedgerDeepSeekConfig& InOutConfig);

    /**
     * @brief 判断配置快照是否满足一次 DeepSeek 请求的最小条件。
     * @param ConfigSnapshot 待检查配置副本。
     * @param bEnabled 功能开关快照。
     * @param OutError 不可用时返回面向日志的错误描述。
     * @return true 表示可以发起模型请求。
     */
    static bool IsConfigUsable(const FVoiceLedgerDeepSeekConfig& ConfigSnapshot,
                               bool bEnabled,
                               std::string* OutError = nullptr);

    /**
     * @brief 从 API 响应中提取 `choices[0].message.content` 文本。
     * @param ResponseJson HTTP 响应 JSON。
     * @param OutContent 成功时返回模型输出文本。
     * @param OutError 失败时返回错误原因。
     */
    bool ExtractDeepSeekContent(const std::string& ResponseJson,
                                std::string& OutContent,
                                std::string& OutError);

    /**
     * @brief 将模型输出的 JSON 字符串解析为结构化条目列表。
     * @param JsonOutput 模型返回的 JSON 文本。
     * @param OutEntries 成功时返回解析结果。
     */
    bool ParseModelOutput(const std::string& JsonOutput,
                          std::vector<FParsedEntry>& OutEntries);

    /**
     * @brief 当模型调用成功但输出无法解析为 JSON 时，按原始语音文本生成一条保底流水。
     * @param RawText 用户原始语音文本。
     * @return 一条金额为 0、分类兜底为“其他支出”、解析完成的条目。
     */
    FParsedEntry BuildFallbackParsedEntry(const std::string& RawText) const;

    /**
     * @brief 将解析后的结构化条目保存到数据库。
     * @param Entries 待保存条目集合。
     * @param LedgerId 目标账本 ID；分类和流水必须严格归属于该账本。
     * @param Username 操作用户名，必须是账本所属家庭成员，并记录为流水创建者。
     * @return true 表示至少一条流水通过授权、分类约束和数据库写入校验。
     * @note 单条保存失败不会回滚其他已成功条目，但会记录错误并继续处理剩余条目。
     */
    bool SaveEntries(const std::vector<FParsedEntry>& Entries,
                     int LedgerId,
                     const std::string& Username);
};
