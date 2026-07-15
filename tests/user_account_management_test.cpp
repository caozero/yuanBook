// user_account_management_test.cpp：用户禁用、密码管理和安全删除领域测试。

#include "AuthSecurity.h"
#include "LedgerManager.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    bool Expect(bool bCondition, const char* Message)
    {
        if (bCondition) return true;
        std::cerr << "[FAIL] " << Message << std::endl;
        return false;
    }
}

int main()
{
    const std::string DbPath = "user_account_management_test.db";
    std::remove(DbPath.c_str());
    std::remove((DbPath + "-wal").c_str());
    std::remove((DbPath + "-shm").c_str());

    bool bOk = true;
    LedgerManager Ledger;
    bOk = Expect(Ledger.Initialize(DbPath), "初始化测试数据库") && bOk;
    bOk = Expect(Ledger.CreateUser("admin-a", AuthSecurity::HashPasswordForStorage("a"), "[\"user\",\"admin\"]"),
                 "创建首个管理员") && bOk;
    bOk = Expect(Ledger.CreateUser("admin-b", AuthSecurity::HashPasswordForStorage("b"), "[\"user\",\"admin\"]"),
                 "创建备用管理员") && bOk;
    bOk = Expect(Ledger.CreateUser("plain", "", "[\"user\"]"),
                 "创建空密码普通用户") && bOk;
    bOk = Expect(Ledger.UpsertAuthSession("plain-token", "plain", 100),
                 "创建普通用户会话") && bOk;

    LedgerManager::FUserAccountMutationResult Result;
    bOk = Expect(!Ledger.SetUserActiveState("admin-a", "admin-a", false, Result)
                     && Result.ErrorCode == "SELF_OPERATION_FORBIDDEN",
                 "禁止管理员禁用自己") && bOk;

    bOk = Expect(Ledger.SetUserActiveState("admin-a", "plain", false, Result),
                 "禁用普通用户") && bOk;
    bOk = Expect(Result.DeletedSessionCount == 1 && !Result.bIsActive,
                 "禁用用户注销全部会话") && bOk;

    std::string Permissions;
    bOk = Expect(!Ledger.AuthenticateUser("plain", AuthSecurity::HashPasswordForStorage("new"), Permissions),
                 "禁用用户不能登录") && bOk;
    bOk = Expect(Ledger.SetUserActiveState("admin-a", "plain", true, Result),
                 "重新启用普通用户") && bOk;
    bOk = Expect(Ledger.AdminUpdateUserPassword("admin-a", "plain", "", Result),
                 "管理员清空普通用户密码") && bOk;

    bOk = Expect(Ledger.SetUserActiveState("admin-a", "admin-b", false, Result),
                 "存在两个管理员时允许禁用其中一个") && bOk;
    bOk = Expect(!Ledger.SetUserActiveState("plain", "admin-a", false, Result)
                     && Result.ErrorCode == "ADMIN_REQUIRED",
                 "普通用户不能执行管理操作") && bOk;
    bOk = Expect(Ledger.DeleteUserAccount("admin-a", "admin-b", true, Result),
                 "已禁用且无业务引用的备用管理员允许物理删除") && bOk;

    bOk = Expect(Ledger.CreateUser("unused", "", "[\"user\"]"),
                 "创建无业务引用用户") && bOk;
    bOk = Expect(Ledger.DeleteUserAccount("admin-a", "unused", true, Result),
                 "无业务引用用户允许物理删除") && bOk;
    std::string UserJson;
    bOk = Expect(!Ledger.GetUserByUsername("unused", UserJson),
                 "物理删除后用户不可查询") && bOk;

    Ledger.Shutdown();
    std::remove(DbPath.c_str());
    std::remove((DbPath + "-wal").c_str());
    std::remove((DbPath + "-shm").c_str());

    if (!bOk) return 1;
    std::cout << "user_account_management_test passed" << std::endl;
    return 0;
}
