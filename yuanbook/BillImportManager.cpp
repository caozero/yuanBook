// BillImportManager.cpp — 通用账单导入管理器实现
// v2: 实例类 + 后台工作线程 + 批量 DeepSeek 分类

#include "BillImportManager.h"
#include "BillImportUtils.h"
#include "LedgerManager.h"
#include "SystemUtils.h"
#include "VoiceLedgerManager.h"
#include "WorkLog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std;

// ============================================================================
// 构造 / 析构
// ============================================================================

BillImportManager::BillImportManager()
{
    srand((unsigned int)time(nullptr));
}

BillImportManager::~BillImportManager()
{
    Shutdown();
}

// ============================================================================
// 初始化 / 关闭
// ============================================================================

bool BillImportManager::Initialize(LedgerManager& Ledger, VoiceLedgerManager& VoiceLedger)
{
    if (m_bRunning.load()) return false;

    m_pLedger = &Ledger;
    m_pVoiceLedger = &VoiceLedger;

    if (!m_pLedger) {
        fprintf(stderr, "[BillImport] ERROR: LedgerManager not set\n");
        return false;
    }

    // 启动工作线程
    m_bRunning.store(true);
    m_WorkerThread = thread(&BillImportManager::WorkerLoop, this);

    printf("[BillImport] Worker thread started\n");
    return true;
}

void BillImportManager::Shutdown()
{
    if (!m_bRunning.load()) return;

    printf("[BillImport] Shutting down...\n");
    m_bRunning.store(false);

    if (m_WorkerThread.joinable()) {
        m_WorkerThread.join();
    }

    printf("[BillImport] Shutdown complete.\n");
}

// ============================================================================
// 工作线程主循环
// ============================================================================

void BillImportManager::WorkerLoop()
{
    printf("[BillImport] Worker loop started\n");

    while (m_bRunning.load()) {
        // 获取待处理的导入任务
        vector<FBillImportTaskItem> tasks;
        if (m_pLedger) {
            m_pLedger->GetPendingBillImportTasks(tasks, 1);
        }

        if (tasks.empty()) {
            // 无任务时休眠 2 秒
            this_thread::sleep_for(chrono::seconds(2));
            continue;
        }

        for (const auto& task : tasks) {
            if (!m_bRunning.load()) break;

            const int userId = m_pLedger ? m_pLedger->GetUserId(task.Username) : 0;
            FScopedWorkLogUserContext logContext(userId > 0 ? userId : 0);

            printf("[BillImport] Processing task %d: file=%s, ledgerId=%d, user=%s\n",
                   task.Id, task.Filename.c_str(), task.LedgerId, task.Username.c_str());

            // 标记处理中
            if (!m_pLedger->MarkBillImportProcessing(task.Id)) {
                fprintf(stderr, "[BillImport] Failed to mark task %d as processing\n", task.Id);
                continue;
            }

            // 处理任务时显式传递账本 ID，不再依赖家庭组兼容镜像字段。
            auto startTime = chrono::steady_clock::now();
            bool ok = ProcessTask(task.Id,
                                  task.Filename,
                                  task.FileContent,
                                  task.LedgerId,
                                  task.Username);
            auto elapsedMs = chrono::duration_cast<chrono::milliseconds>(
                chrono::steady_clock::now() - startTime).count();

            if (ok) {
                printf("[BillImport] Task %d completed in %lld ms\n",
                       task.Id, (long long)elapsedMs);
            } else {
                fprintf(stderr, "[BillImport] Task %d FAILED after %lld ms\n",
                        task.Id, (long long)elapsedMs);
            }
        }
    }

    printf("[BillImport] Worker loop stopped\n");
}

// ============================================================================
// 处理单个导入任务
// ============================================================================

bool BillImportManager::ProcessTask(int TaskId,
                                    const string& Filename,
                                    const string& FileContent,
                                    int LedgerId,
                                    const string& Username)
{
    // ---- 1. 解析 XLSX ----
    vector<FRawRow> rows;
    string error;
    if (!ParseWechatPayXlsx(Filename, FileContent, rows, error)) {
        fprintf(stderr, "[BillImport] Task %d: Parse failed: %s\n", TaskId, error.c_str());
        m_pLedger->MarkBillImportFailed(TaskId, error);
        return false;
    }

    if (rows.empty()) {
        string msg = "未找到可导入的收入或支出流水";
        m_pLedger->MarkBillImportFailed(TaskId, msg);
        return false;
    }

    printf("[BillImport] Task %d: Parsed %zu rows from %s\n",
           TaskId, rows.size(), Filename.c_str());

    // ---- 2. 批量 DeepSeek 分类 ----
    vector<string> categories;
    vector<string> descriptions;

    if (m_pVoiceLedger && m_pVoiceLedger->IsAvailable()) {
        // 收集所有行的 SourceText
        vector<string> sourceTexts;
        sourceTexts.reserve(rows.size());
        for (const auto& row : rows) {
            sourceTexts.push_back(row.SourceText);
        }

        // 一次 DeepSeek 请求完成所有分类；分类查询使用任务发起用户执行账本访问授权。
        if (!m_pVoiceLedger->BatchCategorize(sourceTexts,
                                             LedgerId,
                                             Username,
                                             categories,
                                             descriptions)) {
            printf("[BillImport] Task %d: BatchCategorize failed, using fallback categories\n", TaskId);
            // 不返回失败，使用默认分类继续
        }

        printf("[BillImport] Task %d: BatchCategorize returned %zu categories\n",
               TaskId, categories.size());
    } else {
        printf("[BillImport] Task %d: VoiceLedger not available, using fallback categories\n", TaskId);
    }

    // ---- 3. 写入数据库 ----
    int importedRows = 0;
    int insertedRows = 0;
    int updatedRows = 0;
    int skippedRows = 0;
    string firstError;

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];

        // 使用批量分类结果
        string category;
        string description;

        if (i < categories.size() && !categories[i].empty()) {
            category = categories[i];
        } else {
            category = row.Direction == "收入" ? "其他收入" : "其他支出";
        }

        if (i < descriptions.size() && !descriptions[i].empty()) {
            description = descriptions[i];
        } else {
            description = row.Product.empty() || row.Product == "/" ? row.TradeType : row.Product;
        }

        FLedgerImportedTransaction entry;
        entry.ImportSource = "wechat_pay";
        entry.ImportSourceKey = row.TradeNo;
        entry.ImportRawText = row.SourceText;
        entry.CategoryName = category;
        entry.Amount = row.Amount;
        entry.Type = row.Direction == "收入" ? "income" : "expense";
        entry.Description = description;
        entry.Date = BillImportUtils::DateOnly(row.DateTime);

        bool inserted = false;
        int transId = 0;
        string err;
        if (m_pLedger->UpsertImportedTransaction(LedgerId,
                                                 entry,
                                                 Username,
                                                 inserted,
                                                 transId,
                                                 err)) {
            importedRows++;
            if (inserted) insertedRows++;
            else updatedRows++;
        } else {
            skippedRows++;
            if (firstError.empty()) firstError = err;
        }
    }

    // ---- 4. 标记任务完成 ----
    string sourceType = "wechat_pay";
    string message = "导入完成";
    m_pLedger->MarkBillImportDone(TaskId, (int)rows.size(),
                                  importedRows, insertedRows, updatedRows, skippedRows,
                                  sourceType, message);

    printf("[BillImport] Task %d: Imported=%d, Inserted=%d, Updated=%d, Skipped=%d\n",
           TaskId, importedRows, insertedRows, updatedRows, skippedRows);

    return importedRows > 0;
}

// ============================================================================
// 静态解析方法（无状态，保留原始实现）
// ============================================================================

bool BillImportManager::LooksLikeXlsx(const string& Filename, const string& Content)
{
    return BillImportUtils::LooksLikeXlsx(Filename, Content);
}

bool BillImportManager::ParseWechatPayXlsx(const string& Filename,
                                           const string& Content,
                                           vector<FRawRow>& OutRows,
                                           string& OutError)
{
    OutRows.clear();
    if (!LooksLikeXlsx(Filename, Content)) {
        OutError = "当前只支持 XLSX 格式账单";
        return false;
    }

    string tempDir;
    if (!BillImportUtils::ExtractXlsxBySystemTool(Content, tempDir, OutError)) return false;

    string sharedXml;
    SystemUtils::ReadFileToString(fs::path(tempDir) / "xl" / "sharedStrings.xml", sharedXml);
    vector<string> shared = BillImportUtils::ParseSharedStrings(sharedXml);

    string sheetXml;
    fs::path worksheets = fs::path(tempDir) / "xl" / "worksheets";
    for (int i = 1; i <= 8 && sheetXml.empty(); ++i) {
        SystemUtils::ReadFileToString(worksheets / ("sheet" + to_string(i) + ".xml"), sheetXml);
    }
    if (sheetXml.empty()) {
        OutError = "未找到 XLSX 工作表数据";
        return false;
    }

    vector<vector<string>> rows = BillImportUtils::ParseWorksheetRows(sheetXml, shared);
    map<string, int> cols;
    int headerRow = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        cols.clear();
        for (size_t c = 0; c < rows[i].size(); ++c) {
            string h = SystemUtils::Trim(rows[i][c]);
            if (!h.empty()) cols[h] = (int)c;
        }
        if (cols.count("交易时间") && cols.count("交易类型") && cols.count("收/支") &&
            cols.count("金额(元)") && cols.count("交易单号")) {
            headerRow = (int)i;
            break;
        }
    }
    if (headerRow < 0) {
        OutError = "无法识别账单表头";
        return false;
    }

    for (size_t i = (size_t)headerRow + 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        BillImportUtils::FParsedBillRow parsed;
        parsed.DateTime = BillImportUtils::ExcelSerialToDateTime(BillImportUtils::Cell(row, cols, "交易时间"));
        parsed.TradeType = BillImportUtils::Cell(row, cols, "交易类型");
        parsed.Counterparty = BillImportUtils::Cell(row, cols, "交易对方");
        parsed.Product = BillImportUtils::Cell(row, cols, "商品");
        parsed.Direction = BillImportUtils::Cell(row, cols, "收/支");
        parsed.Amount = BillImportUtils::ParseAmount(BillImportUtils::Cell(row, cols, "金额(元)"));
        parsed.Status = BillImportUtils::Cell(row, cols, "当前状态");
        parsed.TradeNo = BillImportUtils::Cell(row, cols, "交易单号");
        parsed.Remark = BillImportUtils::Cell(row, cols, "备注");
        if (!BillImportUtils::IsWantedDirection(parsed.Direction)) continue;
        if (parsed.TradeNo.empty() || parsed.DateTime.empty() || parsed.Amount <= 0.0) continue;
        parsed.SourceText = BillImportUtils::BuildSourceText(parsed);

        FRawRow rowData;
        rowData.DateTime = parsed.DateTime;
        rowData.TradeType = parsed.TradeType;
        rowData.Counterparty = parsed.Counterparty;
        rowData.Product = parsed.Product;
        rowData.Direction = parsed.Direction;
        rowData.Amount = parsed.Amount;
        rowData.Status = parsed.Status;
        rowData.TradeNo = parsed.TradeNo;
        rowData.Remark = parsed.Remark;
        rowData.SourceText = parsed.SourceText;
        OutRows.push_back(rowData);
    }

    if (OutRows.empty()) {
        OutError = "未找到可导入的收入或支出流水";
        return false;
    }
    return true;
}
