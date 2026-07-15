// auth_security_test.cpp: 认证摘要工具的确定性测试。

#include "AuthSecurity.h"

#include <iostream>
#include <string>

namespace
{
    bool ExpectEqual(const std::string& Actual,
                     const std::string& Expected,
                     const char* CaseName)
    {
        if (Actual == Expected) return true;
        std::cerr << "[FAIL] " << CaseName << std::endl;
        return false;
    }
}

int main()
{
    bool bOk = true;
    bOk = ExpectEqual(AuthSecurity::Sha1Hex("abc"),
                      "a9993e364706816aba3e25717850c26c9cd0d89d",
                      "SHA-1 standard vector") && bOk;
    bOk = ExpectEqual(AuthSecurity::HashPasswordForStorage("secret"),
                      "e5e9fa1ba31ecd1ae84f75caaa474f3a663f05f4",
                      "password storage hash") && bOk;
    bOk = ExpectEqual(AuthSecurity::HashPasswordForStorage(""),
                      "",
                      "empty password keeps setup state") && bOk;
    bOk = ExpectEqual(AuthSecurity::HashPasswordForStorage(" secret "),
                      AuthSecurity::Sha1Hex(" secret "),
                      "password whitespace is preserved") && bOk;
    bOk = (AuthSecurity::Sha1Raw("abc").size() == 20) && bOk;

    if (!bOk) return 1;
    std::cout << "auth_security_test passed" << std::endl;
    return 0;
}
