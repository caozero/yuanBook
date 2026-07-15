// user_password_management_test.cpp: 用户密码更新与会话失效事务测试。

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
    const std::string DbPath = "user_password_management_test.db";
    std::remove(DbPath.c_str());
    std::remove((DbPath + "-wal").c_str());
    std::remove((DbPath + "-shm").c_str());

    bool bOk = true;
    LedgerManager Ledger;
    bOk = Expect(Ledger.Initialize(DbPath), "initialize database") && bOk;
    bOk = Expect(Ledger.CreateUser(
                     "alice",
                     AuthSecurity::HashPasswordForStorage("initial"),
                     "[\"user\"]"),
                 "create user with initial password") && bOk;
    bOk = Expect(Ledger.UpsertAuthSession("token-a", "alice", 100),
                 "create first session") && bOk;
    bOk = Expect(Ledger.UpsertAuthSession("token-b", "alice", 200),
                 "create second session") && bOk;

    int DeletedSessionCount = 0;
    const std::string UpdatedHash = AuthSecurity::HashPasswordForStorage("changed");
    bOk = Expect(Ledger.UpdateUserPasswordAndInvalidateSessions(
                     "alice", UpdatedHash, DeletedSessionCount),
                 "update password and invalidate sessions") && bOk;
    bOk = Expect(DeletedSessionCount == 2, "delete all existing sessions") && bOk;

    std::string StoredHash;
    bOk = Expect(Ledger.GetUserPasswordHash("alice", StoredHash),
                 "read updated password hash") && bOk;
    bOk = Expect(StoredHash == UpdatedHash, "stored hash matches updated password") && bOk;

    std::vector<FLedgerAuthSession> Sessions;
    bOk = Expect(Ledger.LoadAuthSessions(Sessions), "load sessions after password update") && bOk;
    bOk = Expect(Sessions.empty(), "no session survives password update") && bOk;

    bOk = Expect(Ledger.UpsertAuthSession("token-c", "alice", 300),
                 "create session before clearing password") && bOk;
    DeletedSessionCount = 0;
    bOk = Expect(Ledger.UpdateUserPasswordAndInvalidateSessions(
                     "alice", "", DeletedSessionCount),
                 "clear password and invalidate sessions") && bOk;
    bOk = Expect(DeletedSessionCount == 1, "clear password deletes current session") && bOk;
    bOk = Expect(Ledger.GetUserPasswordHash("alice", StoredHash) && StoredHash.empty(),
                 "cleared password enters login setup state") && bOk;

    DeletedSessionCount = 99;
    bOk = Expect(!Ledger.UpdateUserPasswordAndInvalidateSessions(
                      "missing", UpdatedHash, DeletedSessionCount),
                 "missing user update is rejected") && bOk;
    bOk = Expect(DeletedSessionCount == 0, "failed update reports zero deleted sessions") && bOk;

    Ledger.Shutdown();
    std::remove(DbPath.c_str());
    std::remove((DbPath + "-wal").c_str());
    std::remove((DbPath + "-shm").c_str());

    if (!bOk) return 1;
    std::cout << "user_password_management_test passed" << std::endl;
    return 0;
}
