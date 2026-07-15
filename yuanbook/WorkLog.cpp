// WorkLog.cpp — 全局工作日志模块实现

#define WORKLOG_NO_PRINTF_MACROS
#include "WorkLog.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
thread_local int GWorkLogCurrentUserId = 0;
}

class FWorkLogManager::TeeStreamBuf : public std::streambuf
{
public:
    TeeStreamBuf(std::streambuf* Original, FWorkLogManager& Manager)
        : m_Original(Original), m_Manager(Manager)
    {
    }

    ~TeeStreamBuf() override
    {
        sync();
    }

protected:
    int overflow(int Ch) override
    {
        if (Ch == traits_type::eof()) {
            return traits_type::not_eof(Ch);
        }

        const char c = static_cast<char>(Ch);
        if (m_Original) {
            m_Original->sputc(c);
        }

        m_Buffer.push_back(c);
        if (c == '\n') {
            FlushBufferToWorkLog();
        }
        return Ch;
    }

    std::streamsize xsputn(const char* S, std::streamsize Count) override
    {
        if (m_Original) {
            m_Original->sputn(S, Count);
        }

        if (Count > 0) {
            m_Buffer.append(S, static_cast<size_t>(Count));
            size_t pos = 0;
            while ((pos = m_Buffer.find('\n')) != std::string::npos) {
                std::string line = m_Buffer.substr(0, pos + 1);
                m_Manager.Append(line);
                m_Buffer.erase(0, pos + 1);
            }
        }
        return Count;
    }

    int sync() override
    {
        if (m_Original) {
            m_Original->pubsync();
        }
        FlushBufferToWorkLog();
        return 0;
    }

private:
    void FlushBufferToWorkLog()
    {
        if (!m_Buffer.empty()) {
            m_Manager.Append(m_Buffer);
            m_Buffer.clear();
        }
    }

    std::streambuf* m_Original = nullptr;
    FWorkLogManager& m_Manager;
    std::string m_Buffer;
};

FScopedWorkLogUserContext::FScopedWorkLogUserContext(int UserId)
    : m_PreviousUserId(FWorkLogManager::GetCurrentUserId())
{
    FWorkLogManager::SetCurrentUserId(UserId);
}

FScopedWorkLogUserContext::~FScopedWorkLogUserContext()
{
    FWorkLogManager::SetCurrentUserId(m_PreviousUserId);
}

FWorkLogManager& FWorkLogManager::Get()
{
    static FWorkLogManager Instance;
    return Instance;
}

FWorkLogManager::FWorkLogManager() = default;

FWorkLogManager::~FWorkLogManager()
{
    Shutdown();
}

bool FWorkLogManager::Initialize(const std::string& LogDir, int FlushIntervalMs)
{
    std::vector<FWorkLogSubscriptionEntry> notifications;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_bInitialized) {
            return true;
        }

        m_LogDir = LogDir.empty() ? "./log" : LogDir;
        m_FlushIntervalMs = FlushIntervalMs;

        std::error_code ec;
        if (!fs::exists(m_LogDir, ec)) {
            fs::create_directories(m_LogDir, ec);
            if (ec) {
                std::fprintf(stderr, "[WorkLog] failed to create directory %s: %s\n",
                             m_LogDir.c_str(), ec.message().c_str());
                return false;
            }
        }

        fs::path logPath = fs::path(m_LogDir) / (MakeTimestampForFile() + ".log");
        m_LogFilePath = logPath.string();
        m_File = std::fopen(m_LogFilePath.c_str(), "ab");
        if (!m_File) {
            std::fprintf(stderr, "[WorkLog] failed to open log file %s\n", m_LogFilePath.c_str());
            return false;
        }

        m_bInitialized = true;
        AppendLockedLines(0, "[WorkLog] Work log started: " + m_LogFilePath + "\n", notifications);
        std::fflush(m_File);
    }
    NotifySubscribers(notifications);
    return true;
}

void FWorkLogManager::Shutdown()
{
    RestoreStreamRedirects();

    std::vector<FWorkLogSubscriptionEntry> notifications;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_File) {
            AppendLockedLines(0, "[WorkLog] Work log stopped.\n", notifications);
            std::fflush(m_File);
            std::fclose(m_File);
            m_File = nullptr;
        }
        m_bInitialized = false;
    }
    NotifySubscribers(notifications);
}

bool FWorkLogManager::IsInitialized() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_bInitialized;
}

std::string FWorkLogManager::GetLogFilePath() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_LogFilePath;
}

uint64_t FWorkLogManager::GetLatestSeq() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Entries.empty() ? 0 : m_Entries.back().Seq;
}

void FWorkLogManager::Append(const std::string& Text)
{
    AppendForUser(GetCurrentUserId(), Text);
}

void FWorkLogManager::AppendForUser(int UserId, const std::string& Text)
{
    if (Text.empty()) return;

    std::vector<FWorkLogSubscriptionEntry> notifications;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_bInitialized || !m_File) return;
        AppendLockedLines(UserId, Text, notifications);
        std::fflush(m_File);
    }
    NotifySubscribers(notifications);
}

void FWorkLogManager::AppendFormatted(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = std::vsnprintf(nullptr, 0, Format, argsCopy);
    va_end(argsCopy);

    if (needed <= 0) {
        va_end(args);
        return;
    }

    std::vector<char> buffer(static_cast<size_t>(needed) + 1, '\0');
    std::vsnprintf(buffer.data(), buffer.size(), Format, args);
    va_end(args);
    Append(std::string(buffer.data(), static_cast<size_t>(needed)));
}

void FWorkLogManager::AppendFormattedForUser(int UserId, const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = std::vsnprintf(nullptr, 0, Format, argsCopy);
    va_end(argsCopy);

    if (needed <= 0) {
        va_end(args);
        return;
    }

    std::vector<char> buffer(static_cast<size_t>(needed) + 1, '\0');
    std::vsnprintf(buffer.data(), buffer.size(), Format, args);
    va_end(args);
    AppendForUser(UserId, std::string(buffer.data(), static_cast<size_t>(needed)));
}

void FWorkLogManager::Flush()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_File) {
        std::fflush(m_File);
    }
}

std::vector<FWorkLogEntry> FWorkLogManager::GetEntriesSince(uint64_t SinceSeq,
                                                            size_t MaxEntries,
                                                            int UserIdFilter) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<FWorkLogEntry> result;
    for (const auto& entry : m_Entries) {
        if (entry.Seq <= SinceSeq) continue;
        if (UserIdFilter >= 0 && entry.UserId != UserIdFilter) continue;
        result.push_back(entry);
        if (result.size() >= MaxEntries) break;
    }
    return result;
}

uint64_t FWorkLogManager::Subscribe(FWorkLogSubscriber Subscriber)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const uint64_t id = m_NextSubscriptionId++;
    m_Subscribers.emplace_back(id, std::move(Subscriber));
    return id;
}

void FWorkLogManager::Unsubscribe(uint64_t SubscriptionId)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Subscribers.erase(
        std::remove_if(m_Subscribers.begin(), m_Subscribers.end(),
                       [SubscriptionId](const auto& item) { return item.first == SubscriptionId; }),
        m_Subscribers.end());
}

void FWorkLogManager::InstallStreamRedirects()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_CoutBuf || m_CerrBuf) return;

    m_OldCoutBuf = std::cout.rdbuf();
    m_OldCerrBuf = std::cerr.rdbuf();
    m_CoutBuf = new TeeStreamBuf(m_OldCoutBuf, *this);
    m_CerrBuf = new TeeStreamBuf(m_OldCerrBuf, *this);
    std::cout.rdbuf(m_CoutBuf);
    std::cerr.rdbuf(m_CerrBuf);
}

void FWorkLogManager::RestoreStreamRedirects()
{
    TeeStreamBuf* coutBuf = nullptr;
    TeeStreamBuf* cerrBuf = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_OldCoutBuf) std::cout.rdbuf(m_OldCoutBuf);
        if (m_OldCerrBuf) std::cerr.rdbuf(m_OldCerrBuf);
        coutBuf = m_CoutBuf;
        cerrBuf = m_CerrBuf;
        m_CoutBuf = nullptr;
        m_CerrBuf = nullptr;
        m_OldCoutBuf = nullptr;
        m_OldCerrBuf = nullptr;
    }
    delete coutBuf;
    delete cerrBuf;
}

void FWorkLogManager::SetCurrentUserId(int UserId)
{
    GWorkLogCurrentUserId = UserId;
}

int FWorkLogManager::GetCurrentUserId()
{
    return GWorkLogCurrentUserId;
}

std::string FWorkLogManager::MakeTimestampForFile() const
{
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmInfo;
#ifdef _WIN32
    localtime_s(&tmInfo, &tt);
#else
    localtime_r(&tt, &tmInfo);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tmInfo);
    return buf;
}

std::string FWorkLogManager::MakeTimestampForLine() const
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmInfo;
#ifdef _WIN32
    localtime_s(&tmInfo, &tt);
#else
    localtime_r(&tt, &tmInfo);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
    std::ostringstream oss;
    oss << buf << ".";
    oss.width(3);
    oss.fill('0');
    oss << ms.count();
    return oss.str();
}

void FWorkLogManager::AppendLockedLines(int UserId, const std::string& Text,
                                        std::vector<FWorkLogSubscriptionEntry>& OutNotifications)
{
    size_t start = 0;
    while (start < Text.size()) {
        size_t end = Text.find('\n', start);
        bool hasNewline = (end != std::string::npos);
        std::string line = hasNewline ? Text.substr(start, end - start) : Text.substr(start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty()) {
            FWorkLogEntry entry;
            entry.Seq = m_NextSeq++;
            entry.Time = MakeTimestampForLine();
            entry.UserId = UserId;
            entry.Text = line;

            std::ostringstream lineStream;
            lineStream << "[" << entry.Time << "][uid=" << entry.UserId << "] " << entry.Text << "\n";
            const std::string outputLine = lineStream.str();
            if (m_File) {
                std::fwrite(outputLine.data(), 1, outputLine.size(), m_File);
            }

            m_Entries.push_back(entry);
            if (m_Entries.size() > m_MaxMemoryEntries) {
                m_Entries.erase(m_Entries.begin(), m_Entries.begin() + (m_Entries.size() - m_MaxMemoryEntries));
            }

            FWorkLogSubscriptionEntry notifyEntry;
            notifyEntry.Seq = entry.Seq;
            notifyEntry.Time = entry.Time;
            notifyEntry.UserId = entry.UserId;
            notifyEntry.Text = entry.Text;
            OutNotifications.push_back(std::move(notifyEntry));
        }

        if (!hasNewline) break;
        start = end + 1;
    }
}

void FWorkLogManager::NotifySubscribers(const std::vector<FWorkLogSubscriptionEntry>& Notifications)
{
    if (Notifications.empty()) return;

    std::vector<FWorkLogSubscriber> subscribers;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        subscribers.reserve(m_Subscribers.size());
        for (const auto& item : m_Subscribers) {
            subscribers.push_back(item.second);
        }
    }

    for (const auto& entry : Notifications) {
        for (const auto& subscriber : subscribers) {
            if (subscriber) {
                subscriber(entry);
            }
        }
    }
}

int WorkLogPrintf(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    va_list consoleArgs;
    va_copy(consoleArgs, args);
    int ret = std::vfprintf(stdout, Format, consoleArgs);
    va_end(consoleArgs);
    std::fflush(stdout);

    va_list logArgs;
    va_copy(logArgs, args);
    int needed = std::vsnprintf(nullptr, 0, Format, logArgs);
    va_end(logArgs);
    if (needed > 0) {
        std::vector<char> buffer(static_cast<size_t>(needed) + 1, '\0');
        std::vsnprintf(buffer.data(), buffer.size(), Format, args);
        FWorkLogManager::Get().Append(std::string(buffer.data(), static_cast<size_t>(needed)));
    }
    va_end(args);
    return ret;
}

int WorkLogFprintf(FILE* Stream, const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    va_list consoleArgs;
    va_copy(consoleArgs, args);
    int ret = std::vfprintf(Stream, Format, consoleArgs);
    va_end(consoleArgs);
    std::fflush(Stream);

    va_list logArgs;
    va_copy(logArgs, args);
    int needed = std::vsnprintf(nullptr, 0, Format, logArgs);
    va_end(logArgs);
    if (needed > 0) {
        std::vector<char> buffer(static_cast<size_t>(needed) + 1, '\0');
        std::vsnprintf(buffer.data(), buffer.size(), Format, args);
        FWorkLogManager::Get().Append(std::string(buffer.data(), static_cast<size_t>(needed)));
    }
    va_end(args);
    return ret;
}
