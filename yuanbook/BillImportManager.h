// BillImportManager.h — 通用账单导入管理器
// 当前实现一步式上传导入账单，自动识别微信支付 XLSX 账单。
//
// v2: 改为实例类 + 后台工作线程（异步队列模式）
//     工作线程轮询 bill_import_tasks 表，处理待导入任务

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>

class LedgerManager;
class VoiceLedgerManager;

/**
 * @brief 描述一次账单导入任务的汇总结果。
 *
 * 该结构面向“任务完成后的统计展示和状态回传”场景，
 * 不承载原始文件内容，也不包含逐行明细。
 */
struct FBillImportResult
{
    /** 导入器识别到的候选账单总行数。 */
    int TotalRows = 0;

    /** 实际成功写入或更新到数据库的行数。 */
    int ImportedRows = 0;

    /** 本次作为新流水插入数据库的行数。 */
    int InsertedRows = 0;

    /** 本次命中已存在流水并执行更新的行数。 */
    int UpdatedRows = 0;

    /** 因校验失败、落库失败或重复策略而被跳过的行数。 */
    int SkippedRows = 0;

    /** 导入源类型标识，例如 `wechat_pay`。 */
    std::string SourceType;

    /** 可直接展示给前端或写入日志的结果描述。 */
    std::string Message;
};

class BillImportManager
{
public:
    /** 构造导入管理器实例，不立即启动后台线程。 */
    BillImportManager();

    /** 析构时会自动调用 [`BillImportManager::Shutdown()`](../BillImportManager.h:55) 停止后台线程。 */
    ~BillImportManager();

    // ---- 生命周期 ----

    /**
     * @brief 绑定数据库与 AI 分类依赖，并启动后台导入线程。
     *
     * @param Ledger 账本数据库管理器引用，生命周期必须长于当前对象。
     * @param VoiceLedger 语音记账/批量分类管理器引用，生命周期必须长于当前对象。
     * @return true 表示初始化成功；false 表示当前已在运行或依赖无效。
     */
    bool Initialize(LedgerManager& Ledger, VoiceLedgerManager& VoiceLedger);

    /**
     * @brief 请求停止后台线程并等待其退出。
     *
     * 该方法可重复调用；若当前未运行则会直接返回。
     */
    void Shutdown();

public:
    /**
     * @brief 表示从微信支付 XLSX 中解析出的单条原始交易记录。
     *
     * 该结构位于“文件解析完成、AI 分类前”的中间层，
     * 字段命名尽量贴近账单源表头，方便后续检索和调试。
     */
    struct FRawRow
    {
        /** 交易时间，格式通常为 `YYYY-MM-DD HH:MM:SS`。 */
        std::string DateTime;

        /** 交易类型，例如转账、二维码收款、扫码支付。 */
        std::string TradeType;

        /** 交易对方名称，缺失时可能为空。 */
        std::string Counterparty;

        /** 商品或服务名称，部分账单中可能为 `/`。 */
        std::string Product;

        /** 收支方向，当前导入流程只关心“收入”或“支出”。 */
        std::string Direction;

        /** 金额数值，始终存放为正数。 */
        double Amount = 0.0;

        /** 账单状态，例如支付成功、已退款等。 */
        std::string Status;

        /** 交易单号，用作导入幂等键的重要组成部分。 */
        std::string TradeNo;

        /** 备注文本，可能为空或为 `/`。 */
        std::string Remark;

        /** 供 [`VoiceLedgerManager::BatchCategorize()`](yuanbook/VoiceLedgerManager.h:211) 使用的 AI 分类输入文本。 */
        std::string SourceText;
    };

    /**
     * @brief 将微信支付 XLSX 文件解析为原始账单行列表。
     *
     * 该方法不依赖对象状态，适合作为纯解析入口复用。
     * 它只负责识别、解压、读取与筛选账单行，不负责落库。
     *
     * @param Filename 上传文件名，用于辅助判断是否为 XLSX。
     * @param Content 上传文件的原始二进制内容。
     * @param OutRows 成功时返回筛选后的账单行集合。
     * @param OutError 失败时返回可直接展示或记录的错误信息。
     * @return true 表示解析成功；false 表示文件格式非法或内容无法识别。
     */
    static bool ParseWechatPayXlsx(const std::string& Filename,
                                   const std::string& Content,
                                   std::vector<FRawRow>& OutRows,
                                   std::string& OutError);

private:
    // ---- 工作线程 ----
    /**
     * @brief 后台线程主循环。
     *
     * 负责持续轮询 [`bill_import_tasks`](../LedgerManager.h:71) 待处理任务，
     * 并串行调用 [`BillImportManager::ProcessTask()`](../BillImportManager.h:113) 完成导入。
     */
    void WorkerLoop();

    /**
     * @brief 处理单个账单导入任务。
     *
     * 内部流程包含：文件解析、账本级批量 AI 分类、账本级幂等写入、任务状态回写。
     *
     * @param TaskId 账单导入任务 ID。
     * @param Filename 上传文件名。
     * @param FileContent 上传文件原始内容。
     * @param LedgerId 目标账本 ID；分类和导入流水均必须归属于该账本。
     * @param Username 发起导入的用户名，用于家庭成员授权和流水创建者审计。
     * @return true 表示至少成功导入了一部分流水；false 表示整体失败。
     * @note 单行写入失败不会回滚其他成功行，任务结果会记录插入、更新和跳过数量。
     */
    bool ProcessTask(int TaskId,
                     const std::string& Filename,
                     const std::string& FileContent,
                     int LedgerId,
                     const std::string& Username);

    /**
     * @brief 轻量判断上传内容是否像 XLSX 文件。
     *
     * 当前实现委托给 [`BillImportUtils::LooksLikeXlsx()`](../BillImportUtils.h:40)，
     * 在类内保留该包装入口是为了维持业务接口语义集中。
     */
    static bool LooksLikeXlsx(const std::string& Filename,
                              const std::string& Content);

    // ---- 依赖 ----
    /** 非拥有型账本访问指针，由 [`BillImportManager::Initialize()`](../BillImportManager.h:46) 注入。 */
    LedgerManager* m_pLedger = nullptr;

    /** 非拥有型 AI 分类管理器指针，用于批量分类账单行。 */
    VoiceLedgerManager* m_pVoiceLedger = nullptr;

    // ---- 线程控制 ----
    /** 后台账单导入工作线程对象。 */
    std::thread m_WorkerThread;

    /** 工作线程运行标记，`true` 表示轮询循环应继续执行。 */
    std::atomic<bool> m_bRunning{false};
};
