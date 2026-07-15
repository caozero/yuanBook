// WorkLog.h — 全局工作日志模块
// 将控制台输出同时写入控制台、项目根 log 目录中的启动日志文件，
// 并提供结构化内存缓冲、订阅接口、用户上下文与后续管理界面可用的增量读取能力。

#pragma once

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <ostream>

struct FWorkLogEntry
{
    uint64_t Seq = 0;
    std::string Time;
    int UserId = 0;
    std::string Text;
};

struct FWorkLogSubscriptionEntry
{
    uint64_t Seq = 0;
    std::string Time;
    int UserId = 0;
    std::string Text;
};

using FWorkLogSubscriber = std::function<void(const FWorkLogSubscriptionEntry& Entry)>;

class FScopedWorkLogUserContext
{
public:
    explicit FScopedWorkLogUserContext(int UserId);
    ~FScopedWorkLogUserContext();

    FScopedWorkLogUserContext(const FScopedWorkLogUserContext&) = delete;
    FScopedWorkLogUserContext& operator=(const FScopedWorkLogUserContext&) = delete;

private:
    int m_PreviousUserId = 0;
};

class FWorkLogManager
{
public:
    static FWorkLogManager& Get();

    bool Initialize(const std::string& LogDir, int FlushIntervalMs = 0);
    void Shutdown();

    bool IsInitialized() const;
    std::string GetLogFilePath() const;
    uint64_t GetLatestSeq() const;

    void Append(const std::string& Text);
    void AppendForUser(int UserId, const std::string& Text);
    void AppendFormatted(const char* Format, ...);
    void AppendFormattedForUser(int UserId, const char* Format, ...);
    void Flush();

    std::vector<FWorkLogEntry> GetEntriesSince(uint64_t SinceSeq,
                                               size_t MaxEntries = 500,
                                               int UserIdFilter = -1) const;

    uint64_t Subscribe(FWorkLogSubscriber Subscriber);
    void Unsubscribe(uint64_t SubscriptionId);

    void InstallStreamRedirects();
    void RestoreStreamRedirects();

    static void SetCurrentUserId(int UserId);
    static int GetCurrentUserId();

private:
    FWorkLogManager();
    ~FWorkLogManager();
    FWorkLogManager(const FWorkLogManager&) = delete;
    FWorkLogManager& operator=(const FWorkLogManager&) = delete;

    class TeeStreamBuf;

    std::string MakeTimestampForFile() const;
    std::string MakeTimestampForLine() const;
    void AppendLockedLines(int UserId, const std::string& Text,
                           std::vector<FWorkLogSubscriptionEntry>& OutNotifications);
    void NotifySubscribers(const std::vector<FWorkLogSubscriptionEntry>& Notifications);

    mutable std::mutex m_Mutex;
    bool m_bInitialized = false;
    std::string m_LogDir;
    std::string m_LogFilePath;
    FILE* m_File = nullptr;
    int m_FlushIntervalMs = 0;
    uint64_t m_NextSeq = 1;
    uint64_t m_NextSubscriptionId = 1;
    std::vector<FWorkLogEntry> m_Entries;
    size_t m_MaxMemoryEntries = 5000;
    std::vector<std::pair<uint64_t, FWorkLogSubscriber>> m_Subscribers;

    TeeStreamBuf* m_CoutBuf = nullptr;
    TeeStreamBuf* m_CerrBuf = nullptr;
    std::streambuf* m_OldCoutBuf = nullptr;
    std::streambuf* m_OldCerrBuf = nullptr;
};

int WorkLogPrintf(const char* Format, ...);
int WorkLogFprintf(FILE* Stream, const char* Format, ...);

#ifndef WORKLOG_NO_PRINTF_MACROS
    #define printf  WorkLogPrintf
    #define fprintf WorkLogFprintf
#endif
