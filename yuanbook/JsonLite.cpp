#include "JsonLite.h"

#include <cstdlib>

namespace
{
    void AppendUtf8Codepoint(std::string& Out, unsigned int Codepoint)
    {
        if (Codepoint <= 0x7F) {
            Out.push_back(static_cast<char>(Codepoint));
        } else if (Codepoint <= 0x7FF) {
            Out.push_back(static_cast<char>(0xC0 | ((Codepoint >> 6) & 0x1F)));
            Out.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
        } else if (Codepoint <= 0xFFFF) {
            Out.push_back(static_cast<char>(0xE0 | ((Codepoint >> 12) & 0x0F)));
            Out.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
        } else if (Codepoint <= 0x10FFFF) {
            Out.push_back(static_cast<char>(0xF0 | ((Codepoint >> 18) & 0x07)));
            Out.push_back(static_cast<char>(0x80 | ((Codepoint >> 12) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
        }
    }

    int HexValue(char Ch)
    {
        if (Ch >= '0' && Ch <= '9') return Ch - '0';
        if (Ch >= 'a' && Ch <= 'f') return Ch - 'a' + 10;
        if (Ch >= 'A' && Ch <= 'F') return Ch - 'A' + 10;
        return -1;
    }

    size_t FindKeyValueStart(const std::string& Json, const std::string& Key)
    {
        const std::string Search = "\"" + Key + "\"";
        size_t Pos = Json.find(Search);
        if (Pos == std::string::npos) return std::string::npos;

        Pos = Json.find(':', Pos + Search.size());
        if (Pos == std::string::npos) return std::string::npos;

        ++Pos;
        while (Pos < Json.size() && (Json[Pos] == ' ' || Json[Pos] == '\t' ||
               Json[Pos] == '\n' || Json[Pos] == '\r')) {
            ++Pos;
        }
        return Pos;
    }
}

namespace JsonLite
{
    std::string EscapeString(const std::string& Input)
    {
        std::string Result;
        Result.reserve(Input.size() + 16);
        for (char Ch : Input) {
            switch (Ch) {
                case '"':  Result += "\\\""; break;
                case '\\': Result += "\\\\"; break;
                case '\n': Result += "\\n"; break;
                case '\r': Result += "\\r"; break;
                case '\t': Result += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(Ch) >= 0x20) {
                        Result += Ch;
                    }
                    break;
            }
        }
        return Result;
    }

    bool TryGetString(const std::string& Json,
                      const std::string& Key,
                      std::string& OutValue)
    {
        OutValue.clear();
        size_t Pos = FindKeyValueStart(Json, Key);
        if (Pos == std::string::npos || Pos >= Json.size() || Json[Pos] != '"') {
            return false;
        }

        const size_t Start = Pos + 1;
        for (size_t I = Start; I < Json.size(); ++I) {
            if (Json[I] == '\\' && I + 1 < Json.size()) {
                const char Escaped = Json[I + 1];
                switch (Escaped) {
                    case 'n':  OutValue += '\n'; break;
                    case 'r':  OutValue += '\r'; break;
                    case 't':  OutValue += '\t'; break;
                    case '"':  OutValue += '"'; break;
                    case '\\': OutValue += '\\'; break;
                    case 'u': {
                        if (I + 5 >= Json.size()) {
                            OutValue.clear();
                            return false;
                        }
                        int H1 = HexValue(Json[I + 2]);
                        int H2 = HexValue(Json[I + 3]);
                        int H3 = HexValue(Json[I + 4]);
                        int H4 = HexValue(Json[I + 5]);
                        if (H1 < 0 || H2 < 0 || H3 < 0 || H4 < 0) {
                            OutValue.clear();
                            return false;
                        }
                        unsigned int Codepoint = static_cast<unsigned int>((H1 << 12) | (H2 << 8) | (H3 << 4) | H4);
                        AppendUtf8Codepoint(OutValue, Codepoint);
                        I += 4;
                        break;
                    }
                    default:
                        OutValue += Escaped;
                        break;
                }
                ++I;
            } else if (Json[I] == '"') {
                return true;
            } else {
                OutValue += Json[I];
            }
        }

        OutValue.clear();
        return false;
    }

    std::string GetStringOrDefault(const std::string& Json,
                                   const std::string& Key,
                                   const std::string& DefaultValue)
    {
        std::string Value;
        return TryGetString(Json, Key, Value) ? Value : DefaultValue;
    }

    int GetIntOrDefault(const std::string& Json,
                        const std::string& Key,
                        int DefaultValue)
    {
        size_t Pos = FindKeyValueStart(Json, Key);
        if (Pos == std::string::npos || Pos >= Json.size()) return DefaultValue;

        char* End = nullptr;
        long Value = strtol(Json.c_str() + Pos, &End, 10);
        if (End == Json.c_str() + Pos) return DefaultValue;
        return static_cast<int>(Value);
    }

    double GetDoubleOrDefault(const std::string& Json,
                              const std::string& Key,
                              double DefaultValue)
    {
        size_t Pos = FindKeyValueStart(Json, Key);
        if (Pos == std::string::npos || Pos >= Json.size()) return DefaultValue;

        char* End = nullptr;
        double Value = strtod(Json.c_str() + Pos, &End);
        if (End == Json.c_str() + Pos) return DefaultValue;
        return Value;
    }

    bool GetBoolOrDefault(const std::string& Json,
                          const std::string& Key,
                          bool DefaultValue)
    {
        size_t Pos = FindKeyValueStart(Json, Key);
        if (Pos == std::string::npos || Pos >= Json.size()) return DefaultValue;

        if (Json.compare(Pos, 4, "true") == 0) return true;
        if (Json.compare(Pos, 5, "false") == 0) return false;
        if (Json[Pos] == '1') return true;
        if (Json[Pos] == '0') return false;
        return DefaultValue;
    }

    std::string GetArrayRaw(const std::string& Json,
                            const std::string& Key)
    {
        size_t Pos = FindKeyValueStart(Json, Key);
        if (Pos == std::string::npos || Pos >= Json.size() || Json[Pos] != '[') {
            return "";
        }

        int Depth = 1;
        const size_t Start = Pos + 1;
        for (Pos = Start; Pos < Json.size(); ++Pos) {
            if (Json[Pos] == '[') {
                ++Depth;
            } else if (Json[Pos] == ']') {
                --Depth;
                if (Depth == 0) {
                    return Json.substr(Start, Pos - Start);
                }
            }
        }
        return "";
    }

    std::vector<std::string> SplitTopLevelObjects(const std::string& ArrayRaw)
    {
        std::vector<std::string> Result;
        size_t Pos = 0;
        while (Pos < ArrayRaw.size()) {
            while (Pos < ArrayRaw.size() && (ArrayRaw[Pos] == ' ' || ArrayRaw[Pos] == '\t' ||
                   ArrayRaw[Pos] == '\n' || ArrayRaw[Pos] == '\r' || ArrayRaw[Pos] == ',')) {
                ++Pos;
            }
            if (Pos >= ArrayRaw.size()) break;
            if (ArrayRaw[Pos] != '{') {
                ++Pos;
                continue;
            }

            int Depth = 0;
            const size_t Start = Pos;
            for (; Pos < ArrayRaw.size(); ++Pos) {
                if (ArrayRaw[Pos] == '{') {
                    ++Depth;
                } else if (ArrayRaw[Pos] == '}') {
                    --Depth;
                    if (Depth == 0) {
                        ++Pos;
                        Result.push_back(ArrayRaw.substr(Start, Pos - Start));
                        break;
                    }
                }
            }
        }
        return Result;
    }
}
