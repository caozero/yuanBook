#include "SystemUtils.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#define popen  _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SystemUtils
{
    namespace
    {
        static bool IsAbsolutePathString(const std::string& PathValue)
        {
            if (PathValue.empty()) return false;
#ifdef _WIN32
            if (PathValue.size() >= 2 && PathValue[1] == ':') return true;
            if (PathValue[0] == '/' || PathValue[0] == '\\') return true;
#else
            if (PathValue[0] == '/') return true;
#endif
            return false;
        }

        static std::string GetExecutableDirectory()
        {
#ifdef _WIN32
            wchar_t PathBuffer[MAX_PATH];
            if (!GetModuleFileNameW(nullptr, PathBuffer, MAX_PATH)) {
                return "";
            }
            return std::filesystem::path(PathBuffer).parent_path().string();
#else
            char PathBuffer[1024];
            const ssize_t Length = readlink("/proc/self/exe", PathBuffer, sizeof(PathBuffer) - 1);
            if (Length <= 0) {
                return "";
            }
            PathBuffer[Length] = '\0';
            return std::filesystem::path(PathBuffer).parent_path().string();
#endif
        }
    }
    std::string ShellQuote(const std::string& Value)
    {
#ifdef _WIN32
        std::string Result = "\"";
        for (char Ch : Value) {
            if (Ch == '"') {
                Result += "\"\"";
            } else {
                Result += Ch;
            }
        }
        Result += '"';
        return Result;
#else
        std::string Result = "'";
        for (char Ch : Value) {
            if (Ch == '\'') {
                Result += "'\\''";
            } else {
                Result += Ch;
            }
        }
        Result += '\'';
        return Result;
#endif
    }

    std::string MakeTempFilePath(const std::string& Prefix,
                                 const std::string& Extension)
    {
        const auto Now = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path TempDir = std::filesystem::temp_directory_path();
        std::ostringstream Name;
        Name << Prefix << "_" << Now << "_" << rand() << Extension;
        return (TempDir / Name.str()).string();
    }

    bool WriteTextFile(const std::filesystem::path& Path,
                       const std::string& Content)
    {
        std::ofstream File(Path, std::ios::binary);
        if (!File.is_open()) return false;
        File.write(Content.data(), static_cast<std::streamsize>(Content.size()));
        return File.good();
    }

    bool WriteBinaryFile(const std::filesystem::path& Path,
                         const std::string& Content)
    {
        std::ofstream File(Path, std::ios::binary);
        if (!File.is_open()) return false;
        File.write(Content.data(), static_cast<std::streamsize>(Content.size()));
        return File.good();
    }

    bool ReadFileToString(const std::filesystem::path& Path,
                          std::string& OutContent)
    {
        std::ifstream File(Path, std::ios::binary | std::ios::ate);
        if (!File.is_open()) return false;

        const std::streamsize Size = File.tellg();
        if (Size < 0) return false;

        File.seekg(0, std::ios::beg);
        OutContent.assign(static_cast<size_t>(Size), '\0');
        if (Size > 0 && !File.read(&OutContent[0], Size)) return false;
        return true;
    }

    int NormalizeProcessExitCode(int Status)
    {
#ifdef _WIN32
        return Status;
#else
        if (Status == -1) return -1;
        if (WIFEXITED(Status)) return WEXITSTATUS(Status);
        if (WIFSIGNALED(Status)) return 128 + WTERMSIG(Status);
        return Status;
#endif
    }

    bool ReadPipeAll(const std::string& Command,
                     std::string& OutText,
                     int& OutCode)
    {
        OutText.clear();
        OutCode = -1;

#ifdef _WIN32
        const std::string PopenCommand = "\"" + Command + "\"";
#else
        const std::string PopenCommand = Command;
#endif

        FILE* Pipe = popen(PopenCommand.c_str(), "r");
        if (!Pipe) return false;

        char Buffer[4096];
        while (fgets(Buffer, sizeof(Buffer), Pipe)) {
            OutText += Buffer;
        }

        OutCode = NormalizeProcessExitCode(pclose(Pipe));
        return true;
    }

    std::string Trim(const std::string& Value)
    {
        size_t Begin = 0;
        while (Begin < Value.size() && std::isspace(static_cast<unsigned char>(Value[Begin]))) {
            ++Begin;
        }

        size_t End = Value.size();
        while (End > Begin && std::isspace(static_cast<unsigned char>(Value[End - 1]))) {
            --End;
        }

        return Value.substr(Begin, End - Begin);
    }

    std::string ToLowerAscii(std::string Value)
    {
        std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Ch) {
            return static_cast<char>(std::tolower(Ch));
        });
        return Value;
    }

    std::string DecodeXmlEntities(const std::string& Value)
    {
        std::string Result;
        Result.reserve(Value.size());

        for (size_t Index = 0; Index < Value.size(); ++Index) {
            const char Ch = Value[Index];
            if (Ch != '&') {
                Result.push_back(Ch);
                continue;
            }

            const size_t Remaining = Value.size() - Index;
            if (Remaining >= 5 && Value.compare(Index + 1, 4, "amp;") == 0) {
                Result.push_back('&');
                Index += 4;
                continue;
            }
            if (Remaining >= 4 && Value.compare(Index + 1, 3, "lt;") == 0) {
                Result.push_back('<');
                Index += 3;
                continue;
            }
            if (Remaining >= 4 && Value.compare(Index + 1, 3, "gt;") == 0) {
                Result.push_back('>');
                Index += 3;
                continue;
            }
            if (Remaining >= 6 && Value.compare(Index + 1, 5, "quot;") == 0) {
                Result.push_back('"');
                Index += 5;
                continue;
            }
            if (Remaining >= 6 && Value.compare(Index + 1, 5, "apos;") == 0) {
                Result.push_back('\'');
                Index += 5;
                continue;
            }

            Result.push_back(Ch);
        }

        return Result;
    }

    std::string ResolveRuntimePath(const std::string& PathValue,
                                   const std::string& FallbackBaseDir)
    {
        namespace fs = std::filesystem;

        if (PathValue.empty() || IsAbsolutePathString(PathValue)) {
            return PathValue;
        }

        const std::string ExecutableDirectory = GetExecutableDirectory();
        if (!ExecutableDirectory.empty()) {
            fs::path Current = ExecutableDirectory;
            int MaxWalk = 10;
            while (MaxWalk-- > 0) {
                std::error_code ec;
                const fs::path ProjectSourceMarker = Current / "yuanbook" / "YuanBook.cpp";
                if (fs::exists(ProjectSourceMarker, ec)) {
                    const fs::path ProjectCandidate = Current / PathValue;
                    if (fs::exists(ProjectCandidate, ec)) {
                        return fs::absolute(ProjectCandidate).string();
                    }
                    break;
                }

                const fs::path Parent = Current.parent_path();
                if (Parent == Current) {
                    break;
                }
                Current = Parent;
            }
        }

        {
            std::error_code ec;
            if (fs::exists(PathValue, ec)) {
                return fs::absolute(PathValue).string();
            }
        }

        if (!ExecutableDirectory.empty()) {
            fs::path Current = ExecutableDirectory;
            int MaxWalk = 10;
            while (MaxWalk-- > 0) {
                std::error_code ec;
                const fs::path Candidate = Current / PathValue;
                if (fs::exists(Candidate, ec)) {
                    return fs::absolute(Candidate).string();
                }

                const fs::path WwwMarker = Current / "www";
                const fs::path DbMarker = Current / "ledger.db";
                const fs::path SourceMarker = Current / "cpHomeCenter.cpp";
                if (fs::exists(WwwMarker, ec) || fs::exists(DbMarker, ec) || fs::exists(SourceMarker, ec)) {
                    return fs::absolute(Current / PathValue).string();
                }

                const fs::path Parent = Current.parent_path();
                if (Parent == Current) {
                    break;
                }
                Current = Parent;
            }
        }

        if (!FallbackBaseDir.empty()) {
            std::error_code ec;
            const fs::path FallbackCandidate = fs::path(FallbackBaseDir) / PathValue;
            if (fs::exists(FallbackCandidate, ec)) {
                return fs::absolute(FallbackCandidate).string();
            }
            return fs::absolute(FallbackCandidate).string();
        }

        return PathValue;
    }
}
