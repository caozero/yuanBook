#pragma once

#include <map>
#include <string>
#include <vector>

namespace BillImportUtils
{
    /**
     * @brief 表示从 XLSX 工作表中解析得到的一条标准化账单行。
     *
     * 该结构是账单导入阶段的中间数据模型，面向“文件解析完成但尚未落库”的阶段。
     * 它不依赖数据库类型，也不承诺与任何业务实体一一对应。
     */
    struct FParsedBillRow
    {
        std::string DateTime;
        std::string TradeType;
        std::string Counterparty;
        std::string Product;
        std::string Direction;
        double      Amount = 0.0;
        std::string Status;
        std::string TradeNo;
        std::string Remark;
        std::string SourceText;
    };

    /**
     * @brief 判断上传内容是否看起来像 XLSX 文件。
     *
     * 当前策略同时检查：
     * - 文件名扩展名是否为 `.xlsx`
     * - 内容前两个字节是否满足 ZIP 文件头特征 `PK`
     *
     * @param Filename 上传文件名。
     * @param Content 文件原始二进制内容。
     * @return true 表示内容满足当前项目的 XLSX 识别条件；false 表示不满足。
     */
    bool LooksLikeXlsx(const std::string& Filename,
                       const std::string& Content);

    /**
     * @brief 通过系统自带解压工具将 XLSX 内容解包到临时目录。
     *
     * Windows 下优先使用 PowerShell 的 `Expand-Archive`；
     * Unix-like 平台下使用 `unzip`。
     *
     * @param Content XLSX 原始二进制内容。
     * @param OutDir 成功时返回解压后的工作目录路径。
     * @param OutError 失败时返回可直接写日志或返回前端的错误描述。
     * @return true 表示解包成功；false 表示写临时文件失败或系统解压命令执行失败。
     */
    bool ExtractXlsxBySystemTool(const std::string& Content,
                                 std::string& OutDir,
                                 std::string& OutError);

    /**
     * @brief 解析 sharedStrings.xml 中的共享字符串表。
     *
     * XLSX 中大量单元格会通过共享字符串索引引用文本内容。
     * 本函数负责把这些索引映射还原成 UTF-8 字符串列表。
     *
     * @param Xml sharedStrings.xml 原始文本。
     * @return 按索引顺序排列的共享字符串列表。
     */
    std::vector<std::string> ParseSharedStrings(const std::string& Xml);

    /**
     * @brief 将 Excel 单元格引用中的列标记转换为从 0 开始的列索引。
     *
     * 示例：
     * - `A1` -> 0
     * - `B12` -> 1
     * - `AA3` -> 26
     *
     * @param CellReference 单元格引用字符串。
     * @return 从 0 开始的列索引；若无法识别则返回 -1。
     */
    int ColumnIndexFromRef(const std::string& CellReference);

    /**
     * @brief 从 XML 标签文本中读取指定属性值。
     *
     * 这是当前项目 XLSX 轻量解析流程中的字符串扫描工具，
     * 不尝试支持完整 XML 语法。
     *
     * @param Tag 单个 XML 标签文本，例如 `<c r="A1" t="s">`。
     * @param Name 属性名，例如 `r` 或 `t`。
     * @return 属性值；若属性不存在或标签不完整则返回空字符串。
     */
    std::string GetXmlAttr(const std::string& Tag,
                           const std::string& Name);

    /**
     * @brief 解析单个 worksheet XML 为行列字符串矩阵。
     *
     * 输出的二维数组保留行顺序与列顺序，缺失列会用空字符串补位。
     * 共享字符串单元格会自动根据 `SharedStrings` 转换为真实文本。
     *
     * @param Xml worksheet XML 原文。
     * @param SharedStrings 已解析的共享字符串表。
     * @return 工作表的二维字符串矩阵。
     */
    std::vector<std::vector<std::string>> ParseWorksheetRows(
        const std::string& Xml,
        const std::vector<std::string>& SharedStrings);

    /**
     * @brief 从一行工作表数据中按列名提取字段值。
     *
     * @param Row 当前行的单元格数组。
     * @param Columns 列名到列索引的映射表。
     * @param Name 目标列名。
     * @return 去除首尾空白后的字段值；若列不存在则返回空字符串。
     */
    std::string Cell(const std::vector<std::string>& Row,
                     const std::map<std::string, int>& Columns,
                     const std::string& Name);

    /**
     * @brief 从金额文本中提取可转换为浮点数的数值部分。
     *
     * 支持忽略货币符号、逗号与其他非数字字符，只保留 `0-9`、`.`、`-`。
     *
     * @param Value 金额原始文本。
     * @return 解析得到的金额；无法解析时返回 `0.0`。
     */
    double ParseAmount(const std::string& Value);

    /**
     * @brief 从日期时间字符串中截取日期部分。
     *
     * 若输入长度不少于 10，则返回 `YYYY-MM-DD` 前 10 个字符；
     * 否则原样返回。
     *
     * @param DateTime 原始日期时间文本。
     * @return 截断后的日期字符串。
     */
    std::string DateOnly(const std::string& DateTime);

    /**
     * @brief 将 Excel 日期序列号转换为 `YYYY-MM-DD HH:MM:SS` 文本。
     *
     * 若输入本身不是有效序列号，则原样返回，以兼容已经是字符串格式的日期。
     *
     * @param RawValue Excel 单元格原始文本。
     * @return 规范化后的日期时间字符串，或原值。
     */
    std::string ExcelSerialToDateTime(const std::string& RawValue);

    /**
     * @brief 根据账单行内容构建可供 AI 分类使用的 SourceText。
     *
     * 输出内容会将时间、类型、交易对方、商品、收支、金额、状态、交易单号、备注
     * 按稳定键值格式拼接，方便后续批量分类提示词复用。
     *
     * @param Row 标准化账单行。
     * @return 面向 AI 分类和调试日志的紧凑描述文本。
     */
    std::string BuildSourceText(const FParsedBillRow& Row);

    /**
     * @brief 判断一条账单记录是否属于当前导入流程关心的方向。
     *
     * 当前只接受“收入”和“支出”，其他如“中性交易”或空值会被过滤。
     *
     * @param Direction 收支方向文本。
     * @return true 表示该方向应被导入；false 表示应忽略。
     */
    bool IsWantedDirection(const std::string& Direction);
}
