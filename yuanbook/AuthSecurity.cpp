// AuthSecurity.cpp: 认证摘要算法实现。

#include "AuthSecurity.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace
{
    struct FSha1Context
    {
        uint32_t State[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        uint64_t ByteCount = 0;
        uint8_t Buffer[64] = {};
        int BufferIndex = 0;
    };

    uint32_t RotateLeft(uint32_t Value, int BitCount)
    {
        return (Value << BitCount) | (Value >> (32 - BitCount));
    }

    void ProcessBlock(FSha1Context& Context, const uint8_t* Block)
    {
        uint32_t Words[80] = {};
        for (int Index = 0; Index < 16; ++Index) {
            Words[Index] = (static_cast<uint32_t>(Block[Index * 4]) << 24)
                | (static_cast<uint32_t>(Block[Index * 4 + 1]) << 16)
                | (static_cast<uint32_t>(Block[Index * 4 + 2]) << 8)
                | static_cast<uint32_t>(Block[Index * 4 + 3]);
        }
        for (int Index = 16; Index < 80; ++Index) {
            Words[Index] = RotateLeft(
                Words[Index - 3] ^ Words[Index - 8] ^ Words[Index - 14] ^ Words[Index - 16],
                1);
        }

        uint32_t A = Context.State[0];
        uint32_t B = Context.State[1];
        uint32_t C = Context.State[2];
        uint32_t D = Context.State[3];
        uint32_t E = Context.State[4];

        for (int Index = 0; Index < 80; ++Index) {
            uint32_t FunctionValue = 0;
            uint32_t ConstantValue = 0;
            if (Index < 20) {
                FunctionValue = (B & C) | (~B & D);
                ConstantValue = 0x5A827999;
            } else if (Index < 40) {
                FunctionValue = B ^ C ^ D;
                ConstantValue = 0x6ED9EBA1;
            } else if (Index < 60) {
                FunctionValue = (B & C) | (B & D) | (C & D);
                ConstantValue = 0x8F1BBCDC;
            } else {
                FunctionValue = B ^ C ^ D;
                ConstantValue = 0xCA62C1D6;
            }

            const uint32_t Temp = RotateLeft(A, 5) + FunctionValue + E
                + ConstantValue + Words[Index];
            E = D;
            D = C;
            C = RotateLeft(B, 30);
            B = A;
            A = Temp;
        }

        Context.State[0] += A;
        Context.State[1] += B;
        Context.State[2] += C;
        Context.State[3] += D;
        Context.State[4] += E;
    }

    void Update(FSha1Context& Context, const uint8_t* Data, size_t Length)
    {
        for (size_t Index = 0; Index < Length; ++Index) {
            Context.Buffer[Context.BufferIndex++] = Data[Index];
            ++Context.ByteCount;
            if (Context.BufferIndex == 64) {
                ProcessBlock(Context, Context.Buffer);
                Context.BufferIndex = 0;
            }
        }
    }

    void Finalize(FSha1Context& Context, uint8_t OutDigest[20])
    {
        const uint64_t BitCount = Context.ByteCount * 8;
        Context.Buffer[Context.BufferIndex++] = 0x80;

        if (Context.BufferIndex > 56) {
            while (Context.BufferIndex < 64) {
                Context.Buffer[Context.BufferIndex++] = 0;
            }
            ProcessBlock(Context, Context.Buffer);
            Context.BufferIndex = 0;
        }

        while (Context.BufferIndex < 56) {
            Context.Buffer[Context.BufferIndex++] = 0;
        }
        for (int Index = 7; Index >= 0; --Index) {
            Context.Buffer[Context.BufferIndex++] =
                static_cast<uint8_t>((BitCount >> (Index * 8)) & 0xFF);
        }
        ProcessBlock(Context, Context.Buffer);

        for (int Index = 0; Index < 5; ++Index) {
            OutDigest[Index * 4] = static_cast<uint8_t>((Context.State[Index] >> 24) & 0xFF);
            OutDigest[Index * 4 + 1] = static_cast<uint8_t>((Context.State[Index] >> 16) & 0xFF);
            OutDigest[Index * 4 + 2] = static_cast<uint8_t>((Context.State[Index] >> 8) & 0xFF);
            OutDigest[Index * 4 + 3] = static_cast<uint8_t>(Context.State[Index] & 0xFF);
        }
    }
}

namespace AuthSecurity
{
    std::string Sha1Raw(const std::string& Data)
    {
        FSha1Context Context;
        Update(Context, reinterpret_cast<const uint8_t*>(Data.data()), Data.size());

        uint8_t Digest[20] = {};
        Finalize(Context, Digest);
        return std::string(reinterpret_cast<const char*>(Digest), sizeof(Digest));
    }

    std::string Sha1Hex(const std::string& Data)
    {
        const std::string RawDigest = Sha1Raw(Data);
        std::ostringstream Stream;
        Stream << std::hex << std::setfill('0');
        for (const unsigned char Byte : RawDigest) {
            Stream << std::setw(2) << static_cast<int>(Byte);
        }
        return Stream.str();
    }

    std::string HashPasswordForStorage(const std::string& Password)
    {
        // 空密码不生成 SHA-1 常量值，而是保留为空，交由首次登录流程完成初始化。
        return Password.empty() ? std::string() : Sha1Hex(Password);
    }
}
