#include "BillImportUtils.h"

#include "SystemUtils.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace BillImportUtils
{
    bool LooksLikeXlsx(const std::string& Filename,
                       const std::string& Content)
    {
        const std::string Lower = SystemUtils::ToLowerAscii(Filename);
        return Lower.size() >= 5 && Lower.substr(Lower.size() - 5) == ".xlsx" &&
               Content.size() >= 4 && static_cast<unsigned char>(Content[0]) == 0x50 &&
               static_cast<unsigned char>(Content[1]) == 0x4b;
    }

    bool ExtractXlsxBySystemTool(const std::string& Content,
                                 std::string& OutDir,
                                 std::string& OutError)
    {
        OutError.clear();
        const fs::path TempDir = fs::temp_directory_path() /
                                 ("cp_bill_import_" + std::to_string(static_cast<long long>(time(nullptr))) +
                                  "_" + std::to_string(rand()));
        fs::create_directories(TempDir);

        const fs::path XlsxPath = TempDir / "upload.zip";
        if (!SystemUtils::WriteBinaryFile(XlsxPath, Content)) {
            OutError = "无法写入上传临时文件";
            return false;
        }

        const fs::path UnzipDir = TempDir / "xlsx";
        fs::create_directories(UnzipDir);

#ifdef _WIN32
        const std::string Command =
            "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath " +
            SystemUtils::ShellQuote(XlsxPath.string()) + " -DestinationPath " +
            SystemUtils::ShellQuote(UnzipDir.string()) + " -Force\"";
#else
        const std::string Command =
            "unzip -qq -o " + SystemUtils::ShellQuote(XlsxPath.string()) +
            " -d " + SystemUtils::ShellQuote(UnzipDir.string());
#endif

        const int ResultCode = system(Command.c_str());
        if (ResultCode != 0) {
            OutError = "无法解压 XLSX 文件，请确认运行环境可解压 zip 格式账单";
            return false;
        }

        OutDir = UnzipDir.string();
        return true;
    }

    std::vector<std::string> ParseSharedStrings(const std::string& Xml)
    {
        std::vector<std::string> Values;
        size_t Pos = 0;
        while ((Pos = Xml.find("<si", Pos)) != std::string::npos) {
            const size_t End = Xml.find("</si>", Pos);
            if (End == std::string::npos) break;

            const std::string SharedItem = Xml.substr(Pos, End - Pos);
            std::string Text;
            size_t TextPos = 0;
            while ((TextPos = SharedItem.find("<t", TextPos)) != std::string::npos) {
                TextPos = SharedItem.find('>', TextPos);
                if (TextPos == std::string::npos) break;
                const size_t TextEnd = SharedItem.find("</t>", TextPos + 1);
                if (TextEnd == std::string::npos) break;
                Text += SystemUtils::DecodeXmlEntities(SharedItem.substr(TextPos + 1, TextEnd - TextPos - 1));
                TextPos = TextEnd + 4;
            }
            Values.push_back(Text);
            Pos = End + 5;
        }
        return Values;
    }

    int ColumnIndexFromRef(const std::string& CellReference)
    {
        int Index = 0;
        for (char Ch : CellReference) {
            if (Ch >= 'A' && Ch <= 'Z') Index = Index * 26 + (Ch - 'A' + 1);
            else if (Ch >= 'a' && Ch <= 'z') Index = Index * 26 + (Ch - 'a' + 1);
            else break;
        }
        return Index - 1;
    }

    std::string GetXmlAttr(const std::string& Tag,
                           const std::string& Name)
    {
        const std::string Needle = Name + "=\"";
        size_t Pos = Tag.find(Needle);
        if (Pos == std::string::npos) return "";
        Pos += Needle.size();
        const size_t End = Tag.find('"', Pos);
        if (End == std::string::npos) return "";
        return Tag.substr(Pos, End - Pos);
    }

    std::vector<std::vector<std::string>> ParseWorksheetRows(
        const std::string& Xml,
        const std::vector<std::string>& SharedStrings)
    {
        std::vector<std::vector<std::string>> Rows;
        size_t Pos = 0;
        while ((Pos = Xml.find("<row", Pos)) != std::string::npos) {
            const size_t RowEnd = Xml.find("</row>", Pos);
            if (RowEnd == std::string::npos) break;

            const std::string RowXml = Xml.substr(Pos, RowEnd - Pos);
            std::vector<std::string> Row;
            size_t CellPos = 0;
            while ((CellPos = RowXml.find("<c", CellPos)) != std::string::npos) {
                const size_t TagEnd = RowXml.find('>', CellPos);
                if (TagEnd == std::string::npos) break;

                const std::string Tag = RowXml.substr(CellPos, TagEnd - CellPos + 1);
                const size_t CellEnd = RowXml.find("</c>", TagEnd);
                if (CellEnd == std::string::npos) {
                    CellPos = TagEnd + 1;
                    continue;
                }

                const std::string CellXml = RowXml.substr(TagEnd + 1, CellEnd - TagEnd - 1);
                int ColumnIndex = ColumnIndexFromRef(GetXmlAttr(Tag, "r"));
                if (ColumnIndex < 0) ColumnIndex = static_cast<int>(Row.size());
                if (static_cast<int>(Row.size()) <= ColumnIndex) {
                    Row.resize(static_cast<size_t>(ColumnIndex) + 1);
                }

                const std::string CellType = GetXmlAttr(Tag, "t");
                std::string Value;
                size_t ValueStart = CellXml.find("<v>");
                if (ValueStart != std::string::npos) {
                    ValueStart += 3;
                    const size_t ValueEnd = CellXml.find("</v>", ValueStart);
                    if (ValueEnd != std::string::npos) {
                        Value = CellXml.substr(ValueStart, ValueEnd - ValueStart);
                    }
                } else {
                    size_t TextStart = CellXml.find("<t");
                    if (TextStart != std::string::npos) {
                        TextStart = CellXml.find('>', TextStart);
                        const size_t TextEnd = CellXml.find("</t>", TextStart == std::string::npos ? 0 : TextStart);
                        if (TextStart != std::string::npos && TextEnd != std::string::npos) {
                            Value = CellXml.substr(TextStart + 1, TextEnd - TextStart - 1);
                        }
                    }
                }

                if (CellType == "s") {
                    const int SharedIndex = atoi(Value.c_str());
                    Value = (SharedIndex >= 0 && SharedIndex < static_cast<int>(SharedStrings.size()))
                        ? SharedStrings[static_cast<size_t>(SharedIndex)]
                        : "";
                } else {
                    Value = SystemUtils::DecodeXmlEntities(Value);
                }

                Row[static_cast<size_t>(ColumnIndex)] = SystemUtils::Trim(Value);
                CellPos = CellEnd + 4;
            }

            Rows.push_back(Row);
            Pos = RowEnd + 6;
        }
        return Rows;
    }

    std::string Cell(const std::vector<std::string>& Row,
                     const std::map<std::string, int>& Columns,
                     const std::string& Name)
    {
        const auto It = Columns.find(Name);
        if (It == Columns.end() || It->second < 0 || It->second >= static_cast<int>(Row.size())) {
            return "";
        }
        return SystemUtils::Trim(Row[static_cast<size_t>(It->second)]);
    }

    double ParseAmount(const std::string& Value)
    {
        std::string Numeric;
        for (char Ch : Value) {
            if ((Ch >= '0' && Ch <= '9') || Ch == '.' || Ch == '-') {
                Numeric += Ch;
            }
        }
        return Numeric.empty() ? 0.0 : atof(Numeric.c_str());
    }

    std::string DateOnly(const std::string& DateTime)
    {
        if (DateTime.size() >= 10) return DateTime.substr(0, 10);
        return DateTime;
    }

    std::string ExcelSerialToDateTime(const std::string& RawValue)
    {
        char* End = nullptr;
        const double Serial = strtod(RawValue.c_str(), &End);
        if (End == RawValue.c_str() || Serial <= 0.0) {
            return RawValue;
        }

        const long long TotalSeconds = static_cast<long long>((Serial - 25569.0) * 86400.0 + 0.5);
        const time_t TimeValue = static_cast<time_t>(TotalSeconds);

#ifdef _WIN32
        struct tm TimeParts = {0};
        gmtime_s(&TimeParts, &TimeValue);
        const struct tm* TimeInfo = &TimeParts;
#else
        const struct tm* TimeInfo = gmtime(&TimeValue);
#endif
        if (!TimeInfo) return RawValue;

        char Buffer[20];
        snprintf(Buffer, sizeof(Buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                 TimeInfo->tm_year + 1900,
                 TimeInfo->tm_mon + 1,
                 TimeInfo->tm_mday,
                 TimeInfo->tm_hour,
                 TimeInfo->tm_min,
                 TimeInfo->tm_sec);
        return std::string(Buffer);
    }

    std::string BuildSourceText(const FParsedBillRow& Row)
    {
        std::ostringstream Stream;
        Stream << "交易时间=" << Row.DateTime
               << "；交易类型=" << Row.TradeType
               << "；交易对方=" << Row.Counterparty
               << "；商品=" << Row.Product
               << "；收支=" << Row.Direction
               << "；金额=" << Row.Amount
               << "；状态=" << Row.Status
               << "；交易单号=" << Row.TradeNo;
        if (!Row.Remark.empty() && Row.Remark != "/") {
            Stream << "；备注=" << Row.Remark;
        }
        return Stream.str();
    }

    bool IsWantedDirection(const std::string& Direction)
    {
        return Direction == "收入" || Direction == "支出";
    }
}
