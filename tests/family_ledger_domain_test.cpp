// family_ledger_domain_test.cpp — 家庭与账本领域核心行为回归测试

#include "LedgerManager.h"
#include "sqlite3.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    /**
     * @brief 测试断言辅助器，统一记录失败数量并输出可定位信息。
     */
    class FTestContext final
    {
    public:
        void Expect(bool bCondition, const std::string& Message)
        {
            if (bCondition) {
                std::printf("[PASS] %s\n", Message.c_str());
                return;
            }

            ++FailureCount;
            std::fprintf(stderr, "[FAIL] %s\n", Message.c_str());
        }

        int GetFailureCount() const
        {
            return FailureCount;
        }

    private:
        int FailureCount = 0;
    };

    /**
     * @brief 删除测试数据库及 SQLite 可能遗留的 WAL 辅助文件。
     * @param DatabasePath 测试数据库路径。
     * @note 仅操作本测试专用文件名，不接触生产数据库。
     */
    void RemoveDatabaseFiles(const fs::path& DatabasePath)
    {
        std::error_code error;
        fs::remove(DatabasePath, error);
        fs::remove(DatabasePath.string() + "-shm", error);
        fs::remove(DatabasePath.string() + "-wal", error);
    }

    /**
     * @brief 判断扁平 JSON 对象数组中，指定整数 ID 的对象是否包含目标字段片段。
     * @param Json JSON 对象数组文本。
     * @param Id 目标对象 ID。
     * @param ExpectedFragment 目标对象中应出现的字段片段。
     * @return 定位到目标对象且字段片段存在时返回 true。
     * @note 测试数据不存在嵌套对象，因此按最近的右花括号截取即可稳定校验能力字段。
     */
    bool JsonObjectContains(const std::string& Json,
                            int Id,
                            const std::string& ExpectedFragment)
    {
        const std::string idFragment = "\"id\":" + std::to_string(Id);
        const std::size_t objectStart = Json.find(idFragment);
        if (objectStart == std::string::npos) return false;

        const std::size_t objectEnd = Json.find('}', objectStart);
        if (objectEnd == std::string::npos) return false;

        return Json.substr(objectStart, objectEnd - objectStart + 1)
                   .find(ExpectedFragment) != std::string::npos;
    }

    /**
     * @brief 执行返回单个整数的 SQL 查询。
     * @param DatabasePath 数据库文件路径。
     * @param Sql 不包含外部输入的测试 SQL。
     * @param OutValue 输出查询结果。
     * @return 成功读取首行首列整数时返回 true。
     */
    bool QueryScalarInt(const fs::path& DatabasePath,
                        const std::string& Sql,
                        int& OutValue)
    {
        OutValue = 0;
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(DatabasePath.string().c_str(),
                            &database,
                            SQLITE_OPEN_READONLY,
                            nullptr) != SQLITE_OK) {
            if (database != nullptr) {
                sqlite3_close(database);
            }
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, Sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }

        const bool bSucceeded = sqlite3_step(statement) == SQLITE_ROW;
        if (bSucceeded) {
            OutValue = sqlite3_column_int(statement, 0);
        }

        sqlite3_finalize(statement);
        sqlite3_close(database);
        return bSucceeded;
    }
}

int main()
{
    const fs::path databasePath = fs::current_path() / "family_ledger_domain_test.db";
    RemoveDatabaseFiles(databasePath);

    FTestContext test;
    LedgerManager manager;
    std::string error;

    test.Expect(manager.Initialize(databasePath.string()),
                "初始化独立 Schema version 2 测试数据库");
    test.Expect(manager.CreateUser("owner", "owner-hash", "[\"user\"]"),
                "创建家庭发起人用户");
    test.Expect(manager.CreateUser("member", "member-hash", "[\"user\"]"),
                "创建普通家庭成员用户");
    test.Expect(manager.CreateUser("outsider", "outsider-hash", "[\"user\"]"),
                "创建家庭外用户");
    test.Expect(manager.CreateUser("system-admin", "admin-hash", "[\"user\",\"admin\"]"),
                "创建系统管理员用户");

    int familyId = 0;
    test.Expect(manager.CreateFamily("测试家庭", "owner", familyId, error),
                "创建家庭并原子写入创建者 owner 关系");
    test.Expect(familyId > 0, "创建家庭返回有效家庭 ID");
    test.Expect(manager.IsFamilyOwner(familyId, "owner"),
                "家庭创建者是 owner");
    test.Expect(!manager.IsFamilyMember(familyId, "member"),
                "未加入用户不具备家庭成员身份");

    int scalarValue = -1;
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM family_members WHERE family_id="
                                   + std::to_string(familyId)
                                   + " AND role='owner';",
                               scalarValue)
                    && scalarValue == 1,
                "新家庭恰好有一个 owner");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM account_categories;",
                               scalarValue)
                    && scalarValue == 0,
                "创建家庭不会提前生成账本默认分类");

    int firstLedgerId = 0;
    test.Expect(manager.CreateLedger(familyId,
                                     "日常账本",
                                     "owner",
                                     firstLedgerId,
                                     error),
                "家庭 owner 创建首个账本");
    test.Expect(firstLedgerId > 0, "创建账本返回有效账本 ID");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM account_categories WHERE ledger_id="
                                   + std::to_string(firstLedgerId) + ";",
                               scalarValue)
                    && scalarValue == 11,
                "创建账本生成 11 个且仅属于该账本的默认分类");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM account_categories WHERE ledger_id<>"
                                   + std::to_string(firstLedgerId) + ";",
                               scalarValue)
                    && scalarValue == 0,
                "默认分类没有泄漏到其他账本作用域");

    test.Expect(manager.AddFamilyMember(familyId,
                                        "member",
                                        "member",
                                        "owner",
                                        error),
                "家庭 owner 添加普通成员");
    test.Expect(manager.IsFamilyMember(familyId, "member"),
                "新增用户具备家庭成员身份");
    test.Expect(manager.CanAccessLedger(firstLedgerId, "member"),
                "普通成员继承家庭下账本访问权");
    test.Expect(!manager.CanManageLedger(firstLedgerId, "member"),
                "普通成员不具备账本管理权");
    test.Expect(!manager.CanAccessLedger(firstLedgerId, "outsider"),
                "家庭外用户不能访问账本");

    std::string categoriesJson;
    error.clear();
    test.Expect(manager.GetLedgerCategories(firstLedgerId,
                                            "",
                                            "member",
                                            categoriesJson,
                                            error),
                "普通成员可读取家庭账本分类");
    test.Expect(categoriesJson.find("\"ledgerId\":" + std::to_string(firstLedgerId))
                    != std::string::npos,
                "分类查询结果显式返回所属账本 ID");

    int memberCreatedCategoryId = 0;
    error.clear();
    test.Expect(!manager.CreateLedgerCategory(firstLedgerId,
                                              "成员越权分类",
                                              "expense",
                                              0,
                                              10,
                                              "member",
                                              memberCreatedCategoryId,
                                              error),
                "普通成员不能创建账本分类");
    test.Expect(memberCreatedCategoryId == 0,
                "分类越权创建失败时不返回脏 ID");

    int customCategoryId = 0;
    error.clear();
    test.Expect(manager.CreateLedgerCategory(firstLedgerId,
                                             "家庭娱乐",
                                             "expense",
                                             0,
                                             20,
                                             "owner",
                                             customCategoryId,
                                             error),
                "家庭 owner 可创建账本一级分类");
    test.Expect(customCategoryId > 0,
                "创建自定义分类返回有效分类 ID");

    int categoryLedgerId = 0;
    test.Expect(manager.GetCategoryLedgerId(customCategoryId, categoryLedgerId)
                    && categoryLedgerId == firstLedgerId,
                "分类所属账本查询返回正确 ledger_id");

    int childCategoryId = 0;
    error.clear();
    test.Expect(manager.CreateLedgerCategory(firstLedgerId,
                                             "亲子活动",
                                             "income",
                                             customCategoryId,
                                             21,
                                             "owner",
                                             childCategoryId,
                                             error),
                "家庭 owner 可创建同账本子分类");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM account_categories WHERE id="
                                   + std::to_string(childCategoryId)
                                   + " AND ledger_id=" + std::to_string(firstLedgerId)
                                   + " AND parent_id=" + std::to_string(customCategoryId)
                                   + " AND type='expense';",
                               scalarValue)
                    && scalarValue == 1,
                "子分类强制继承父分类类型且保持账本隔离");

    error.clear();
    test.Expect(!manager.UpdateLedgerCategory(customCategoryId,
                                              "家庭娱乐",
                                              "expense",
                                              childCategoryId,
                                              20,
                                              "owner",
                                              error),
                "禁止分类将自身后代设为父分类");
    error.clear();
    test.Expect(!manager.DeleteLedgerCategory(customCategoryId, "owner", error),
                "仍有子分类的一级分类不能删除");
    error.clear();
    test.Expect(!manager.DeleteLedgerCategory(childCategoryId, "member", error),
                "普通成员不能删除分类");
    error.clear();
    test.Expect(manager.DeleteLedgerCategory(childCategoryId, "owner", error),
                "家庭 owner 可删除无子分类的自定义分类");
    error.clear();
    test.Expect(manager.DeleteLedgerCategory(customCategoryId, "owner", error),
                "子分类清理后 owner 可删除一级自定义分类");

    int protectedCategoryId = 0;
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT id FROM account_categories WHERE ledger_id="
                                   + std::to_string(firstLedgerId)
                                   + " AND name='其他支出' AND type='expense' LIMIT 1;",
                               protectedCategoryId)
                    && protectedCategoryId > 0,
                "定位账本级受保护兜底分类");
    error.clear();
    test.Expect(!manager.DeleteLedgerCategory(protectedCategoryId, "owner", error),
                "即使是 owner 也不能删除账本兜底分类");

    int unauthorizedLedgerId = 0;
    error.clear();
    test.Expect(!manager.CreateLedger(familyId,
                                      "越权账本",
                                      "member",
                                      unauthorizedLedgerId,
                                      error),
                "普通成员不能创建账本");
    test.Expect(unauthorizedLedgerId == 0,
                "越权创建账本失败时不返回脏 ID");

    int secondLedgerId = 0;
    test.Expect(manager.CreateLedger(familyId,
                                     "备用账本",
                                     "owner",
                                     secondLedgerId,
                                     error),
                "家庭 owner 创建第二个账本");

    int secondLedgerParentId = 0;
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT id FROM account_categories WHERE ledger_id="
                                   + std::to_string(secondLedgerId)
                                   + " AND name='餐饮' LIMIT 1;",
                               secondLedgerParentId)
                    && secondLedgerParentId > 0,
                "定位第二账本的一级分类");
    int crossLedgerChildId = 0;
    error.clear();
    test.Expect(!manager.CreateLedgerCategory(firstLedgerId,
                                              "跨账本子分类",
                                              "expense",
                                              secondLedgerParentId,
                                              30,
                                              "owner",
                                              crossLedgerChildId,
                                              error),
                "禁止引用其他账本的父分类");
    test.Expect(crossLedgerChildId == 0,
                "跨账本分类创建失败时不返回脏 ID");

    FLedgerImportedTransaction importedEntry;
    importedEntry.ImportSource = "domain_test";
    importedEntry.ImportSourceKey = "import-001";
    importedEntry.ImportRawText = "测试导入原文";
    importedEntry.CategoryName = "餐饮";
    importedEntry.Amount = 22.0;
    importedEntry.Type = "expense";
    importedEntry.Description = "测试导入流水";
    importedEntry.Date = "2026-07-09";

    bool importedInserted = false;
    int importedTransactionId = 0;
    error.clear();
    test.Expect(manager.UpsertImportedTransaction(firstLedgerId,
                                                  importedEntry,
                                                  "member",
                                                  importedInserted,
                                                  importedTransactionId,
                                                  error)
                    && importedInserted
                    && importedTransactionId > 0,
                "普通成员可向家庭账本导入流水且首次写入标记为新增");
    importedEntry.Amount = 23.0;
    int updatedImportedTransactionId = 0;
    error.clear();
    test.Expect(manager.UpsertImportedTransaction(firstLedgerId,
                                                  importedEntry,
                                                  "member",
                                                  importedInserted,
                                                  updatedImportedTransactionId,
                                                  error)
                    && !importedInserted
                    && updatedImportedTransactionId == importedTransactionId,
                "导入流水按账本和来源键幂等更新");
    error.clear();
    test.Expect(!manager.UpsertImportedTransaction(firstLedgerId,
                                                   importedEntry,
                                                   "outsider",
                                                   importedInserted,
                                                   updatedImportedTransactionId,
                                                   error),
                "家庭外用户不能向账本导入流水");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM transactions WHERE id="
                                   + std::to_string(importedTransactionId)
                                   + " AND ledger_id=" + std::to_string(firstLedgerId)
                                   + " AND amount=23.0;",
                               scalarValue)
                    && scalarValue == 1,
                "导入流水写入 ledger_id 并完成同账本幂等更新");

    int voiceTransactionId = 0;
    error.clear();
    test.Expect(manager.CreateVoiceInputTransaction(firstLedgerId,
                                                    "早餐 18 元",
                                                    "member",
                                                    voiceTransactionId,
                                                    error)
                    && voiceTransactionId > 0,
                "普通成员可创建账本级待解析语音流水");
    int unauthorizedVoiceTransactionId = 0;
    error.clear();
    test.Expect(!manager.CreateVoiceInputTransaction(firstLedgerId,
                                                     "越权语音流水",
                                                     "outsider",
                                                     unauthorizedVoiceTransactionId,
                                                     error),
                "家庭外用户不能创建语音流水");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM transactions WHERE id="
                                   + std::to_string(voiceTransactionId)
                                   + " AND ledger_id=" + std::to_string(firstLedgerId)
                                   + " AND is_voice_input=1 AND voice_parse_status='pending';",
                               scalarValue)
                    && scalarValue == 1,
                "语音流水使用 ledger_id 且保持待解析状态");

    const int importTaskId = manager.CreateBillImportTask(firstLedgerId,
                                                           "member",
                                                           "test.csv",
                                                           "csv-content");
    test.Expect(importTaskId > 0,
                "普通成员可创建账本级导入任务");
    test.Expect(manager.CreateBillImportTask(firstLedgerId,
                                             "outsider",
                                             "unauthorized.csv",
                                             "csv-content") == 0,
                "家庭外用户不能创建账本导入任务");
    FBillImportTaskItem importTask;
    test.Expect(manager.GetBillImportTaskStatus(importTaskId, importTask)
                    && importTask.LedgerId == firstLedgerId,
                "导入任务状态显式返回目标账本 ID");

    int ownerTransactionId = 0;
    error.clear();
    test.Expect(manager.CreateLedgerTransaction(firstLedgerId,
                                                protectedCategoryId,
                                                88.5,
                                                "expense",
                                                "owner 创建的流水",
                                                "owner",
                                                "2026-07-10",
                                                ownerTransactionId,
                                                error),
                "家庭 owner 可在账本中新增流水");
    test.Expect(ownerTransactionId > 0,
                "owner 新增流水返回有效流水 ID");

    int memberTransactionId = 0;
    error.clear();
    test.Expect(manager.CreateLedgerTransaction(firstLedgerId,
                                                protectedCategoryId,
                                                35.0,
                                                "expense",
                                                "member 创建的流水",
                                                "member",
                                                "2026-07-10",
                                                memberTransactionId,
                                                error),
                "普通成员可在家庭账本中新增流水");
    test.Expect(memberTransactionId > 0,
                "member 新增流水返回有效流水 ID");

    int outsiderTransactionId = 0;
    error.clear();
    test.Expect(!manager.CreateLedgerTransaction(firstLedgerId,
                                                 protectedCategoryId,
                                                 1.0,
                                                 "expense",
                                                 "越权流水",
                                                 "outsider",
                                                 "2026-07-10",
                                                 outsiderTransactionId,
                                                 error),
                "家庭外用户不能新增账本流水");
    test.Expect(outsiderTransactionId == 0,
                "越权新增流水失败时不返回脏 ID");

    int crossLedgerTransactionId = 0;
    error.clear();
    test.Expect(!manager.CreateLedgerTransaction(firstLedgerId,
                                                 secondLedgerParentId,
                                                 12.0,
                                                 "expense",
                                                 "跨账本分类流水",
                                                 "member",
                                                 "2026-07-10",
                                                 crossLedgerTransactionId,
                                                 error),
                "新增流水时禁止引用其他账本分类");

    int transactionLedgerId = 0;
    test.Expect(manager.GetTransactionLedgerId(memberTransactionId, transactionLedgerId)
                    && transactionLedgerId == firstLedgerId,
                "流水所属账本查询返回正确 ledger_id");

    std::string statsJson;
    error.clear();
    test.Expect(manager.GetLedgerStats(firstLedgerId,
                                       "member",
                                       "2026-07-10",
                                       "2026-07-10",
                                       "day",
                                       statsJson,
                                       error),
                "普通成员可查询账本级统计");
    test.Expect(statsJson.find("\"ledgerId\":" + std::to_string(firstLedgerId)) != std::string::npos
                    && statsJson.find("\"totalExpense\":123.5") != std::string::npos
                    && statsJson.find("\"balance\":-123.5") != std::string::npos,
                "账本统计按 ledger_id 和日期范围汇总普通流水");

    std::string unauthorizedStatsJson;
    error.clear();
    test.Expect(!manager.GetLedgerStats(firstLedgerId,
                                        "outsider",
                                        "",
                                        "",
                                        "day",
                                        unauthorizedStatsJson,
                                        error),
                "家庭外用户不能查询账本统计");
    test.Expect(unauthorizedStatsJson == "{}",
                "账本统计越权失败时清空输出数据");

    std::string crossLedgerCategoryStatsJson;
    error.clear();
    test.Expect(!manager.GetLedgerStats(firstLedgerId,
                                        "member",
                                        "",
                                        "",
                                        "category",
                                        crossLedgerCategoryStatsJson,
                                        error,
                                        secondLedgerParentId),
                "账本统计禁止使用其他账本的父分类钻取");

    std::string transactionsJson;
    int transactionTotal = 0;
    error.clear();
    test.Expect(manager.GetLedgerTransactions(firstLedgerId,
                                              "member",
                                              "",
                                              "",
                                              0,
                                              "",
                                              0,
                                              50,
                                              transactionsJson,
                                              transactionTotal,
                                              error),
                "普通成员可查看家庭账本全部流水");
    test.Expect(transactionTotal == 4,
                "账本流水查询返回普通、导入和语音流水的正确总数");
    test.Expect(JsonObjectContains(transactionsJson,
                                   memberTransactionId,
                                   "\"canEdit\":true")
                    && JsonObjectContains(transactionsJson,
                                          memberTransactionId,
                                          "\"canDelete\":true"),
                "member 对自己创建的流水获得编辑和删除能力");
    test.Expect(JsonObjectContains(transactionsJson,
                                   ownerTransactionId,
                                   "\"canEdit\":false")
                    && JsonObjectContains(transactionsJson,
                                          ownerTransactionId,
                                          "\"canDelete\":false"),
                "member 对其他成员创建的流水不获得编辑和删除能力");
    test.Expect(JsonObjectContains(transactionsJson,
                                   memberTransactionId,
                                   "\"ledgerId\":" + std::to_string(firstLedgerId)),
                "流水查询结果显式返回所属账本 ID");

    std::string unauthorizedTransactionsJson;
    int unauthorizedTransactionTotal = -1;
    error.clear();
    test.Expect(!manager.GetLedgerTransactions(firstLedgerId,
                                               "outsider",
                                               "",
                                               "",
                                               0,
                                               "",
                                               0,
                                               50,
                                               unauthorizedTransactionsJson,
                                               unauthorizedTransactionTotal,
                                               error),
                "家庭外用户不能查看账本流水");
    test.Expect(unauthorizedTransactionsJson == "[]"
                    && unauthorizedTransactionTotal == 0,
                "流水越权查询失败时清空输出数据");

    error.clear();
    test.Expect(!manager.UpdateLedgerTransaction(ownerTransactionId,
                                                 protectedCategoryId,
                                                 90.0,
                                                 "expense",
                                                 "member 越权修改",
                                                 "2026-07-11",
                                                 "member",
                                                 error),
                "member 不能修改 owner 创建的流水");
    error.clear();
    test.Expect(manager.UpdateLedgerTransaction(memberTransactionId,
                                                protectedCategoryId,
                                                36.0,
                                                "expense",
                                                "member 修改自己的流水",
                                                "2026-07-11",
                                                "member",
                                                error),
                "member 可以修改自己创建的流水");
    error.clear();
    test.Expect(manager.UpdateLedgerTransaction(memberTransactionId,
                                                protectedCategoryId,
                                                37.0,
                                                "expense",
                                                "owner 管理 member 流水",
                                                "2026-07-12",
                                                "owner",
                                                error),
                "owner 可以修改 member 创建的流水");
    error.clear();
    test.Expect(!manager.UpdateLedgerTransaction(memberTransactionId,
                                                 secondLedgerParentId,
                                                 38.0,
                                                 "expense",
                                                 "跨账本分类修改",
                                                 "2026-07-12",
                                                 "owner",
                                                 error),
                "修改流水时禁止引用其他账本分类");

    error.clear();
    test.Expect(!manager.DeleteLedgerTransaction(ownerTransactionId,
                                                 "member",
                                                 error),
                "member 不能删除 owner 创建的流水");
    error.clear();
    test.Expect(manager.DeleteLedgerTransaction(memberTransactionId,
                                                "member",
                                                error),
                "member 可以删除自己创建的流水");

    int memberTransactionForOwnerDeleteId = 0;
    error.clear();
    test.Expect(manager.CreateLedgerTransaction(firstLedgerId,
                                                protectedCategoryId,
                                                16.0,
                                                "expense",
                                                "等待 owner 删除",
                                                "member",
                                                "2026-07-13",
                                                memberTransactionForOwnerDeleteId,
                                                error),
                "member 可再次创建用于 owner 管理验证的流水");
    error.clear();
    test.Expect(manager.DeleteLedgerTransaction(memberTransactionForOwnerDeleteId,
                                                "owner",
                                                error),
                "owner 可以删除 member 创建的流水");

    error.clear();
    test.Expect(manager.GetLedgerTransactions(firstLedgerId,
                                              "owner",
                                              "",
                                              "",
                                              0,
                                              "",
                                              0,
                                              50,
                                              transactionsJson,
                                              transactionTotal,
                                              error)
                    && transactionTotal == 3
                    && JsonObjectContains(transactionsJson,
                                          ownerTransactionId,
                                          "\"canEdit\":true")
                    && JsonObjectContains(transactionsJson,
                                          ownerTransactionId,
                                          "\"canDelete\":true"),
                "owner 查询流水时具备全部流水管理能力");

    const int64_t inviteExpiresAt = static_cast<int64_t>(std::time(nullptr)) + 3600;
    error.clear();
    test.Expect(manager.SaveInviteRegistration("JOIN",
                                               true,
                                               0,
                                               firstLedgerId,
                                               "member",
                                               "owner",
                                               true,
                                               inviteExpiresAt,
                                               error),
                "家庭 owner 可创建自动加入家庭的注册邀请并由默认账本推导家庭");

    FLedgerInviteRegistration currentInvite;
    error.clear();
    test.Expect(manager.GetCurrentInviteRegistration("owner",
                                                     true,
                                                     0,
                                                     firstLedgerId,
                                                     "member",
                                                     currentInvite,
                                                     error)
                    && currentInvite.FamilyId == familyId
                    && currentInvite.DefaultLedgerId == firstLedgerId,
                "当前邀请查询使用与保存一致的服务端家庭推导");

    FLedgerInviteRegistration consumedInvite;
    error.clear();
    test.Expect(manager.RegisterUserWithInvite("JOIN",
                                              "invited-member",
                                              "invited-hash",
                                              "[\"user\"]",
                                              consumedInvite,
                                              error),
                "邀请码注册原子创建用户并加入家庭");
    test.Expect(manager.IsFamilyMember(familyId, "invited-member")
                    && !manager.IsFamilyOwner(familyId, "invited-member"),
                "member 注册邀请按服务端角色写入家庭成员关系");
    int invitedCurrentFamilyId = 0;
    int invitedCurrentLedgerId = 0;
    test.Expect(manager.GetCurrentFamilyId("invited-member", invitedCurrentFamilyId)
                    && invitedCurrentFamilyId == familyId
                    && manager.GetCurrentLedgerId("invited-member", invitedCurrentLedgerId)
                    && invitedCurrentLedgerId == firstLedgerId,
                "注册邀请设置服务端记录中的当前家庭与默认账本");

    error.clear();
    test.Expect(!manager.SaveInviteRegistration("MEMB",
                                                false,
                                                0,
                                                0,
                                                "member",
                                                "member",
                                                true,
                                                inviteExpiresAt,
                                                error),
                "普通 member 即使运行时参数开启也不能创建独立注册邀请");
    error.clear();
    test.Expect(!manager.SaveInviteRegistration("LOCK",
                                                false,
                                                0,
                                                0,
                                                "member",
                                                "owner",
                                                false,
                                                inviteExpiresAt,
                                                error)
                    && error == "只有系统管理员才能创建独立用户",
                "运行时参数关闭时家庭 owner 不能创建独立注册邀请");
    error.clear();
    test.Expect(manager.SaveInviteRegistration("FREE",
                                               false,
                                               0,
                                               0,
                                               "member",
                                               "system-admin",
                                               false,
                                               inviteExpiresAt,
                                               error),
                "系统 admin 始终可以创建独立注册邀请");

    error.clear();
    test.Expect(manager.SaveInviteRegistration("ADJN",
                                               true,
                                               0,
                                               firstLedgerId,
                                               "member",
                                               "system-admin",
                                               false,
                                               inviteExpiresAt,
                                               error),
                "系统 admin 从账本作用域创建的邀请仍然是加入当前家庭的邀请");
    FLedgerInviteRegistration adminLedgerInvite;
    error.clear();
    test.Expect(manager.RegisterUserWithInvite("ADJN",
                                               "admin-ledger-invited-user",
                                               "admin-ledger-invited-hash",
                                               "[\"user\"]",
                                               adminLedgerInvite,
                                               error)
                    && adminLedgerInvite.bAutoJoinFamily
                    && adminLedgerInvite.FamilyId == familyId
                    && adminLedgerInvite.DefaultLedgerId == firstLedgerId,
                "管理员账本入口邀请注册时保留服务端推导的家庭与默认账本作用域");
    test.Expect(manager.IsFamilyMember(familyId, "admin-ledger-invited-user"),
                "管理员账本入口邀请的新用户必须加入当前账本所属家庭");

    FLedgerInviteRegistration independentInvite;
    error.clear();
    test.Expect(manager.RegisterUserWithInvite("FREE",
                                              "independent-user",
                                              "independent-hash",
                                              "[\"user\"]",
                                              independentInvite,
                                              error)
                    && !independentInvite.bAutoJoinFamily,
                "独立邀请原子创建不加入家庭的用户");
    test.Expect(!manager.IsFamilyMember(familyId, "independent-user"),
                "独立邀请不会写入任何家庭成员关系");

    error.clear();
    test.Expect(!manager.RegisterUserWithInvite("JOIN",
                                               "member",
                                               "duplicate-hash",
                                               "[\"user\"]",
                                               consumedInvite,
                                               error),
                "原子注册在用户名冲突时整体失败");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM family_members fm JOIN users u ON u.id=fm.user_id "
                               "WHERE fm.family_id=" + std::to_string(familyId)
                                   + " AND u.username='member';",
                               scalarValue)
                    && scalarValue == 1,
                "原子注册失败不会产生重复或脏家庭成员关系");

    // 成员邀请必须完整运行在 V2 家庭模型上，禁止回退读取已删除的旧组表。
    FLedgerMemberInvite memberInvite;
    error.clear();
    test.Expect(manager.CreateMemberInvite(firstLedgerId,
                                           "outsider",
                                           "member",
                                           "owner",
                                           3600,
                                           memberInvite,
                                           error)
                    && memberInvite.Base.Id > 0
                    && memberInvite.Base.ActionType == "family_member_invite"
                    && memberInvite.FamilyId == familyId,
                "家庭 owner 通过账本作用域创建 V2 家庭成员邀请");
    const int acceptedInviteId = memberInvite.Base.Id;

    FLedgerMemberInvite duplicatedInvite;
    error.clear();
    test.Expect(manager.CreateMemberInvite(firstLedgerId,
                                           "outsider",
                                           "admin",
                                           "owner",
                                           7200,
                                           duplicatedInvite,
                                           error)
                    && duplicatedInvite.Base.Id == acceptedInviteId
                    && duplicatedInvite.Role == "member",
                "重复邀请复用未过期记录且不会授予 owner 角色");

    std::string inviteJson;
    error.clear();
    test.Expect(manager.GetIncomingMemberInvites("outsider", inviteJson, error)
                    && JsonObjectContains(inviteJson,
                                          acceptedInviteId,
                                          "\"familyId\":" + std::to_string(familyId)),
                "被邀请用户可查询 incoming 家庭邀请");
    error.clear();
    test.Expect(manager.GetSentMemberInvites(firstLedgerId, "owner", inviteJson, error)
                    && JsonObjectContains(inviteJson,
                                          acceptedInviteId,
                                          "\"targetUsername\":\"outsider\""),
                "家庭 owner 可按账本查询 sent 家庭邀请");

    FLedgerMemberInvite unauthorizedInvite;
    error.clear();
    test.Expect(!manager.CreateMemberInvite(firstLedgerId,
                                            "system-admin",
                                            "member",
                                            "member",
                                            3600,
                                            unauthorizedInvite,
                                            error),
                "普通家庭 member 无权发送成员邀请");

    int acceptedLedgerId = 0;
    error.clear();
    test.Expect(manager.AcceptMemberInvite(acceptedInviteId,
                                           "outsider",
                                           acceptedLedgerId,
                                           error)
                    && acceptedLedgerId == firstLedgerId
                    && manager.IsFamilyMember(familyId, "outsider"),
                "接受邀请原子写入家庭成员关系并返回默认账本");
    int outsiderFamilyId = 0;
    int outsiderLedgerId = 0;
    test.Expect(manager.GetCurrentFamilyId("outsider", outsiderFamilyId)
                    && outsiderFamilyId == familyId
                    && manager.GetCurrentLedgerId("outsider", outsiderLedgerId)
                    && outsiderLedgerId == firstLedgerId,
                "接受邀请同步更新用户当前家庭与账本上下文");
    error.clear();
    test.Expect(!manager.AcceptMemberInvite(acceptedInviteId,
                                            "outsider",
                                            acceptedLedgerId,
                                            error),
                "已接受邀请不能重复接受");

    FLedgerMemberInvite cancellableInvite;
    error.clear();
    test.Expect(manager.CreateMemberInvite(firstLedgerId,
                                           "system-admin",
                                           "member",
                                           "owner",
                                           3600,
                                           cancellableInvite,
                                           error),
                "创建用于取消流程的家庭成员邀请");
    error.clear();
    test.Expect(manager.CancelMemberInvite(cancellableInvite.Base.Id, "owner", error),
                "邀请发起人可取消待处理邀请");
    error.clear();
    test.Expect(!manager.AcceptMemberInvite(cancellableInvite.Base.Id,
                                            "system-admin",
                                            acceptedLedgerId,
                                            error),
                "已取消邀请不能接受");

    FLedgerMemberInvite rejectableInvite;
    error.clear();
    test.Expect(manager.CreateMemberInvite(firstLedgerId,
                                           "system-admin",
                                           "member",
                                           "owner",
                                           3600,
                                           rejectableInvite,
                                           error),
                "取消后可重新创建新的待处理邀请");
    error.clear();
    test.Expect(manager.RejectMemberInvite(rejectableInvite.Base.Id,
                                           "system-admin",
                                           error),
                "邀请目标用户可拒绝待处理邀请");

    test.Expect(manager.SetCurrentLedgerId("member", secondLedgerId, error),
                "普通成员可切换到家庭下其他账本");

    int currentFamilyId = 0;
    int currentLedgerId = 0;
    test.Expect(manager.GetCurrentFamilyId("member", currentFamilyId)
                    && currentFamilyId == familyId,
                "切换账本时同步设置当前家庭上下文");
    test.Expect(manager.GetCurrentLedgerId("member", currentLedgerId)
                    && currentLedgerId == secondLedgerId,
                "当前账本上下文持久化为目标账本");
    test.Expect(manager.SetCurrentFamilyId("member", familyId, error),
                "家庭成员可切换当前家庭");
    test.Expect(manager.GetCurrentLedgerId("member", currentLedgerId)
                    && currentLedgerId == firstLedgerId,
                "切换家庭时按稳定顺序修复为家庭首个账本");

    std::string pidBindingJson;
    FLedgerPidRoute pidRoute;
    error.clear();
    test.Expect(manager.CreateLedgerPidBinding("member", "v3-member-pid-a",
                                               secondLedgerId, pidBindingJson, error),
                "家庭成员可创建显式绑定账本的 PID");
    test.Expect(manager.CreateLedgerPidBinding("member", "v3-member-pid-b",
                                               secondLedgerId, pidBindingJson, error),
                "同一用户允许多个 PID 指向同一账本");
    test.Expect(manager.SetCurrentLedgerId("member", firstLedgerId, error)
                    && manager.ResolveLedgerPidRoute("v3-member-pid-a", pidRoute, error)
                    && pidRoute.CreatorUsername == "member"
                    && pidRoute.LedgerId == secondLedgerId,
                "PID 路由精确使用绑定账本且不受当前账本切换影响");
    test.Expect(manager.IsGroupAdmin(secondLedgerId, "owner"),
                "兼容管理权限由账本所属家庭 owner 角色推导");
    test.Expect(!manager.IsGroupAdmin(secondLedgerId, "member"),
                "普通家庭 member 不会被兼容接口误判为账本管理员");

    int resolvedFamilyId = 0;
    test.Expect(manager.GetLedgerFamilyId(secondLedgerId, resolvedFamilyId)
                    && resolvedFamilyId == familyId,
                "兼容 Web 路径可安全解析账本所属家庭");

    error.clear();
    test.Expect(manager.DeleteGroup(secondLedgerId, "owner", error),
                "家庭 owner 可通过兼容接口删除 V2 账本");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM families WHERE id="
                                   + std::to_string(familyId) + ";",
                               scalarValue)
                    && scalarValue == 1,
                "删除账本不会误删其所属家庭");
    test.Expect(manager.GetCurrentLedgerId("member", currentLedgerId)
                    && currentLedgerId == firstLedgerId,
                "删除当前账本后自动回退到同家庭其他账本");
    pidRoute = FLedgerPidRoute{};
    error.clear();
    test.Expect(!manager.ResolveLedgerPidRoute("v3-member-pid-a", pidRoute, error),
                "账本删除后绑定 PID 级联删除且不会漂移到其他账本");

    error.clear();
    test.Expect(!manager.UpdateFamilyMemberRole(familyId,
                                                "owner",
                                                "member",
                                                "owner",
                                                error),
                "禁止降级家庭最后一个 owner");
    error.clear();
    test.Expect(!manager.RemoveFamilyMember(familyId,
                                            "owner",
                                            "owner",
                                            error),
                "禁止移除家庭最后一个 owner");

    test.Expect(manager.UpdateFamilyMemberRole(familyId,
                                               "member",
                                               "owner",
                                               "owner",
                                               error),
                "现有 owner 可提升普通成员为 owner");
    test.Expect(manager.IsFamilyOwner(familyId, "member"),
                "被提升成员获得 owner 角色");
    test.Expect(manager.RemoveFamilyMember(familyId,
                                           "owner",
                                           "member",
                                           error),
                "存在多个 owner 时允许移除其中一个 owner");
    test.Expect(!manager.IsFamilyMember(familyId, "owner"),
                "被移除 owner 不再拥有家庭成员身份");
    test.Expect(!manager.CanAccessLedger(firstLedgerId, "owner"),
                "被移除 owner 不再能够访问家庭账本");

    currentFamilyId = -1;
    currentLedgerId = -1;
    test.Expect(!manager.GetCurrentFamilyId("owner", currentFamilyId)
                    && currentFamilyId == 0,
                "移除成员时清理其当前家庭上下文");
    test.Expect(!manager.GetCurrentLedgerId("owner", currentLedgerId)
                    && currentLedgerId == 0,
                "移除成员时清理其当前账本上下文");

    error.clear();
    test.Expect(!manager.UpdateFamilyMemberRole(familyId,
                                                "member",
                                                "member",
                                                "member",
                                                error),
                "新的最后一个 owner 仍不能自我降级");
    error.clear();
    test.Expect(!manager.RemoveFamilyMember(familyId,
                                            "member",
                                            "member",
                                            error),
                "新的最后一个 owner 仍不能退出家庭");
    test.Expect(QueryScalarInt(databasePath,
                               "SELECT COUNT(*) FROM family_members WHERE family_id="
                                   + std::to_string(familyId)
                                   + " AND role='owner';",
                               scalarValue)
                    && scalarValue == 1,
                "所有保护操作结束后家庭仍保留一个 owner");

    manager.Shutdown();
    RemoveDatabaseFiles(databasePath);

    if (test.GetFailureCount() != 0) {
        std::fprintf(stderr,
                     "[RESULT] family_ledger_domain_test failed: %d assertion(s)\n",
                     test.GetFailureCount());
        return 1;
    }

    std::printf("[RESULT] family_ledger_domain_test passed\n");
    return 0;
}
