// ===========================================================================
// tests/test_check.h
//
// 测试用的最小断言宏，两个测试程序共用
// ===========================================================================

#pragma once

#include <cstdio>
#include <string>

// C++17 的 inline 变量：多个测试文件都包含本头文件时，链接器只保留一份
inline int g_passed = 0;
inline int g_failed = 0;

// 条件不成立就打印"哪个文件、哪一行、期望什么"
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (cond) {                                                                      \
            ++g_passed;                                                                  \
        } else {                                                                         \
            ++g_failed;                                                                  \
            std::printf("  [失败] %s:%d  条件不成立: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                                \
    } while (false)

// 比整数。失败时把两边的值打出来，光看"不相等"没法排查
#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        const long long _actual = static_cast<long long>(actual);                        \
        const long long _expected = static_cast<long long>(expected);                    \
        if (_actual == _expected) {                                                      \
            ++g_passed;                                                                  \
        } else {                                                                         \
            ++g_failed;                                                                  \
            std::printf("  [失败] %s:%d  期望 %lld，实际 %lld\n", __FILE__, __LINE__,    \
                        _expected, _actual);                                             \
        }                                                                                \
    } while (false)

// 比字符串。字节块里可能有 \t \r 和不可打印字符，打印时原样输出，终端看着怪但能对照
#define CHECK_EQ_STR(actual, expected)                                                   \
    do {                                                                                 \
        const std::string _actual = (actual);                                            \
        const std::string _expected = (expected);                                        \
        if (_actual == _expected) {                                                      \
            ++g_passed;                                                                  \
        } else {                                                                         \
            ++g_failed;                                                                  \
            std::printf("  [失败] %s:%d  长度 期望 %d 实际 %d\n", __FILE__, __LINE__,    \
                        static_cast<int>(_expected.size()),                              \
                        static_cast<int>(_actual.size()));                               \
            std::printf("    期望：[%s]\n    实际：[%s]\n", _expected.c_str(),           \
                        _actual.c_str());                                                \
        }                                                                                \
    } while (false)
