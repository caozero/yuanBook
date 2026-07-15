#pragma once

#include <string>
#include <vector>

namespace JsonLite
{
    /**
     * @brief 将普通 UTF-8 字符串转义为可安全嵌入 JSON 字符串字面量的内容。
     *
     * 该函数不会添加外围双引号，只返回转义后的正文内容。
     * 适用于手工拼接 JSON 请求体、日志快照或数据库中间 JSON 文本。
     *
     * @param Input 原始 UTF-8 文本，可以包含换行、制表符、双引号和反斜杠。
     * @return 已完成 JSON 转义的 UTF-8 文本，不包含外围双引号。
     */
    std::string EscapeString(const std::string& Input);

    /**
     * @brief 尝试从 JSON 对象文本中读取指定 key 对应的字符串字段。
     *
     * 行为特点：
     * - 仅适用于当前项目中使用的轻量级 JSON 文本扫描场景。
     * - 要求目标字段的值是 JSON 字符串。
     * - 能处理常见转义字符，包括 \n、\r、\t、\"、\\ 以及 \uXXXX。
     * - 读取失败时返回 false，并清空 OutValue。
     *
     * @param Json JSON 对象原文。
     * @param Key  需要读取的字段名，不含双引号。
     * @param OutValue 成功时写入解码后的 UTF-8 字符串，失败时清空。
     * @return true 表示字段存在且成功按字符串解析；false 表示字段缺失、类型不符或文本不完整。
     */
    bool TryGetString(const std::string& Json,
                      const std::string& Key,
                      std::string& OutValue);

    /**
     * @brief 读取字符串字段；若字段缺失或无法解析，则返回默认值。
     *
     * 适用于“字段可选、失败可兜底”的业务逻辑，例如读取配置项、轻量接口参数等。
     *
     * @param Json JSON 对象原文。
     * @param Key 字段名，不含双引号。
     * @param DefaultValue 默认返回值。
     * @return 成功解析时返回字符串字段值，否则返回 DefaultValue。
     */
    std::string GetStringOrDefault(const std::string& Json,
                                   const std::string& Key,
                                   const std::string& DefaultValue = "");

    /**
     * @brief 读取整数值字段；若字段缺失或无法解析，则返回默认值。
     *
     * @param Json JSON 对象原文。
     * @param Key 字段名，不含双引号。
     * @param DefaultValue 默认整数值。
     * @return 解析得到的整数值，或 DefaultValue。
     */
    int GetIntOrDefault(const std::string& Json,
                        const std::string& Key,
                        int DefaultValue = 0);

    /**
     * @brief 读取浮点数字段；若字段缺失或无法解析，则返回默认值。
     *
     * @param Json JSON 对象原文。
     * @param Key 字段名，不含双引号。
     * @param DefaultValue 默认浮点值。
     * @return 解析得到的浮点值，或 DefaultValue。
     */
    double GetDoubleOrDefault(const std::string& Json,
                              const std::string& Key,
                              double DefaultValue = 0.0);

    /**
     * @brief 读取布尔字段；兼容 true、false、1、0 四种文本表示。
     *
     * @param Json JSON 对象原文。
     * @param Key 字段名，不含双引号。
     * @param DefaultValue 默认布尔值。
     * @return 解析得到的布尔值，或 DefaultValue。
     */
    bool GetBoolOrDefault(const std::string& Json,
                          const std::string& Key,
                          bool DefaultValue = false);

    /**
     * @brief 提取指定数组字段的原始内容，不包含最外层方括号。
     *
     * 例如输入 `{ "items": [ {"a":1}, {"b":2} ] }`，返回
     * ` {"a":1}, {"b":2} `。
     *
     * @param Json JSON 对象原文。
     * @param Key 数组字段名，不含双引号。
     * @return 数组内部原始内容；若字段不存在、不是数组或语法不完整，则返回空字符串。
     */
    std::string GetArrayRaw(const std::string& Json,
                            const std::string& Key);

    /**
     * @brief 将 JSON 数组内部文本拆分为顶层对象列表。
     *
     * 输入文本应来自 [`JsonLite::GetArrayRaw()`](../JsonLite.h:101) 或等价内容，
     * 其格式为多个顶层对象用逗号分隔，但不含最外层 `[` 和 `]`。
     *
     * @param ArrayRaw JSON 数组内部原始文本。
     * @return 逐个顶层对象的原始字符串列表；无法识别的片段会被跳过。
     */
    std::vector<std::string> SplitTopLevelObjects(const std::string& ArrayRaw);
}
