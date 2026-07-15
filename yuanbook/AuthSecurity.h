// AuthSecurity.h: 认证摘要算法的统一入口。
//
// 该模块只负责无状态的摘要计算，不持有用户、数据库或会话状态。
// 当前 SHA-1 算法用于兼容既有密码存储格式、Challenge-Response 登录协议
// 以及 WebSocket RFC 6455 握手流程；调用方不得将摘要结果视为可逆加密数据。

#pragma once

#include <string>

namespace AuthSecurity
{
    /**
     * @brief 计算输入数据的 SHA-1 原始二进制摘要。
     * @param Data 待计算的数据，可包含任意字节，也允许为空。
     * @return 固定 20 字节的二进制摘要；返回值可能包含空字符。
     * @note 该函数无外部副作用且线程安全，主要供 WebSocket 握手等协议层逻辑使用。
     */
    std::string Sha1Raw(const std::string& Data);

    /**
     * @brief 计算输入数据的 SHA-1 小写十六进制摘要。
     * @param Data 待计算的数据，可包含任意字节，也允许为空。
     * @return 固定 40 个字符的小写十六进制摘要。
     * @note 该函数无外部副作用且线程安全；当前密码存储与挑战响应协议均依赖此格式。
     */
    std::string Sha1Hex(const std::string& Data);

    /**
     * @brief 将明文密码转换为当前系统兼容的持久化密码摘要。
     * @param Password 用户提供的原始密码；不会裁剪首尾空白。
     * @return 非空密码对应的 40 字符 SHA-1 小写十六进制摘要；空密码返回空字符串。
     * @note 空字符串具有“账号等待用户在登录页初始化密码”的业务语义，调用方必须显式区分
     *       “未提供密码参数”和“明确要求清空密码”。函数不会记录或保存明文密码。
     */
    std::string HashPasswordForStorage(const std::string& Password);
}
