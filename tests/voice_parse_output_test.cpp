#define private public
#include "VoiceLedgerManager.h"
#undef private

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    bool ExpectSingleZeroAmount(const std::string& Name,
                                const std::string& Json,
                                const std::string& ExpectedCategory)
    {
        VoiceLedgerManager manager;
        std::vector<FParsedEntry> entries;
        if (!manager.ParseModelOutput(Json, entries)) {
            std::fprintf(stderr, "[FAIL] %s: ParseModelOutput returned false\n", Name.c_str());
            return false;
        }
        if (entries.size() != 1) {
            std::fprintf(stderr, "[FAIL] %s: expected 1 entry, got %zu\n", Name.c_str(), entries.size());
            return false;
        }
        const FParsedEntry& entry = entries[0];
        if (!entry.ParseCompleted) {
            std::fprintf(stderr, "[FAIL] %s: ParseCompleted is false\n", Name.c_str());
            return false;
        }
        if (entry.Amount != 0.0) {
            std::fprintf(stderr, "[FAIL] %s: expected amount 0, got %.2f\n", Name.c_str(), entry.Amount);
            return false;
        }
        if (entry.Type != "expense") {
            std::fprintf(stderr, "[FAIL] %s: expected expense, got %s\n", Name.c_str(), entry.Type.c_str());
            return false;
        }
        if (entry.CategoryName != ExpectedCategory) {
            std::fprintf(stderr, "[FAIL] %s: expected category %s, got %s\n",
                         Name.c_str(), ExpectedCategory.c_str(), entry.CategoryName.c_str());
            return false;
        }
        std::printf("[PASS] %s\n", Name.c_str());
        return true;
    }
    bool ExpectFallbackEntry(const std::string& Name,
                             const std::string& RawText,
                             const std::string& ExpectedDescription)
    {
        VoiceLedgerManager manager;
        FParsedEntry entry = manager.BuildFallbackParsedEntry(RawText);
        if (!entry.ParseCompleted) {
            std::fprintf(stderr, "[FAIL] %s: ParseCompleted is false\n", Name.c_str());
            return false;
        }
        if (entry.Amount != 0.0) {
            std::fprintf(stderr, "[FAIL] %s: expected amount 0, got %.2f\n", Name.c_str(), entry.Amount);
            return false;
        }
        if (entry.Type != "expense") {
            std::fprintf(stderr, "[FAIL] %s: expected expense, got %s\n", Name.c_str(), entry.Type.c_str());
            return false;
        }
        if (entry.CategoryName != "其他支出") {
            std::fprintf(stderr, "[FAIL] %s: expected category 其他支出, got %s\n",
                         Name.c_str(), entry.CategoryName.c_str());
            return false;
        }
        if (entry.Description != ExpectedDescription) {
            std::fprintf(stderr, "[FAIL] %s: expected description %s, got %s\n",
                         Name.c_str(), ExpectedDescription.c_str(), entry.Description.c_str());
            return false;
        }
        std::printf("[PASS] %s\n", Name.c_str());
        return true;
    }
}

int main()
{
    int failed = 0;

    if (!ExpectSingleZeroAmount(
            "array_amount_zero_parse_completed",
            "[{\"category\":\"其他支出\",\"amount\":0,\"type\":\"expense\",\"description\":\"买了一个东西\",\"parseCompleted\":true}]",
            "其他支出")) {
        ++failed;
    }

    if (!ExpectSingleZeroAmount(
            "array_missing_amount_parse_completed",
            "[{\"category\":\"其他支出\",\"type\":\"expense\",\"description\":\"买了一个东西\",\"parseCompleted\":true}]",
            "其他支出")) {
        ++failed;
    }

    if (!ExpectSingleZeroAmount(
            "single_object_missing_amount",
            "{\"category\":\"其他支出\",\"type\":\"expense\",\"description\":\"买了一个东西\",\"parseCompleted\":true}",
            "其他支出")) {
        ++failed;
    }

    if (!ExpectSingleZeroAmount(
            "missing_category_fallback",
            "[{\"amount\":0,\"type\":\"expense\",\"description\":\"买了一个东西\",\"parseCompleted\":true}]",
            "其他支出")) {
        ++failed;
    }

    if (!ExpectFallbackEntry(
            "fallback_unparseable_model_output",
            "测试测试",
            "测试测试")) {
        ++failed;
    }

    if (!ExpectFallbackEntry(
            "fallback_blank_voice_text",
            "   ",
            "未填写金额的语音记录")) {
        ++failed;
    }

    return failed == 0 ? 0 : 1;
}
