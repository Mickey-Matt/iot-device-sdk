// ===========================================================================
// tests/test_json.cpp
//
// 【为什么简历项目一定要有测试？】
// 因为面试官问"你怎么保证代码是对的"时，
// 答"我手动跑了几遍"和答"我写了 12 个自动化测试用例，覆盖了边界情况"
// 是两个完全不同层次的回答，后者说明你有工程意识
//
// 这里不引入 doctest/Catch2，而是手写 20 行断言宏 ——
// 目的是让你看清"测试框架本质上是什么"：不过是自动比较 + 统计失败数
// 等你看懂了这 20 行，再去用 doctest 就是水到渠成
// ===========================================================================

#include <cstdio>
#include <string>

#include "iotd/device_client.h"
#include "iotd/json.h"

namespace {

int g_passed = 0;
int g_failed = 0;

// 断言宏：条件不成立就打印"哪个文件、哪一行、期望什么"
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::printf("  [失败] %s:%d  条件不成立: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                                          \
    } while (false)

// 比较两个 double 是否近似相等
// 【重要】浮点数不能用 == 比较！
//   因为 0.1 + 0.2 != 0.3 在计算机里是真的
//   正确做法是判断两者差值是否小于一个极小值（epsilon）
bool NearlyEqual(double a, double b, double eps = 1e-9) {
    const double diff = (a > b) ? (a - b) : (b - a);
    return diff < eps;
}

void TestValidMessage() {
    std::printf("[测试] 正常报文解析\n");
    const std::string frame = R"({"device_id":"fish-tank-01","timestamp_ms":1700000000000,)"
                              R"("temp":25.6,"humid":62.5,"light_lux":420})";

    iotd::DeviceMessage msg;
    std::string err;
    CHECK(iotd::DeviceClient::ParseFrame(frame, msg, err));
    CHECK(err.empty());
    CHECK(msg.device_id == "fish-tank-01");
    CHECK(NearlyEqual(msg.reading.temperature_c, 25.6));
    CHECK(NearlyEqual(msg.reading.humidity_pct, 62.5));
    CHECK(msg.reading.light_lux == 420);
    CHECK(msg.timestamp_ms == 1700000000000LL);
}

void TestJsonBasics() {
    std::printf("[测试] JSON 基础类型\n");

    CHECK(iotd::json::Parse(R"("hello")").AsString() == "hello");
    CHECK(NearlyEqual(iotd::json::Parse("42").AsNumber(), 42.0));
    CHECK(NearlyEqual(iotd::json::Parse("-3.5").AsNumber(), -3.5));
    CHECK(iotd::json::Parse("true").AsBool() == true);
    CHECK(iotd::json::Parse("false").AsBool() == false);

    // 嵌套对象
    const auto nested = iotd::json::Parse(R"({"a":{"b":{"c":7}}})");
    CHECK(nested.IsObject());
    const auto& inner = std::get<iotd::json::Object>(nested.data).at("a");
    CHECK(iotd::json::GetNumber(std::get<iotd::json::Object>(inner.data).at("b"), "c", -1) == 7.0);

    // 数组
    const auto arr = iotd::json::Parse("[1, 2, 3]");
    CHECK(arr.IsArray());
    CHECK(std::get<iotd::json::Array>(arr.data).size() == 3);

    // 空对象与空数组
    CHECK(iotd::json::Parse("{}").IsObject());
    CHECK(iotd::json::Parse("[]").IsArray());
}

void TestEscapeSequences() {
    std::printf("[测试] 转义字符\n");
    const auto v = iotd::json::Parse(R"({"msg":"line1\nline2\tend","q":"say \"hi\""})");
    CHECK(iotd::json::GetString(v, "msg") == "line1\nline2\tend");
    CHECK(iotd::json::GetString(v, "q") == "say \"hi\"");
}

void TestParseErrors() {
    std::printf("[测试] 错误处理（坏数据不能让程序崩溃）\n");

    // 每一项都必须是"抛异常"而不是"崩溃"或"静默返回错误值"
    const char* bad_cases[] = {
        R"({"a":1)",          // 缺右花括号 —— 模拟串口半包
        R"({"a":})",          // 缺值
        R"({a:1})",           // 键没加引号
        R"({"a":1,})",        // 多余的逗号
        R"("unterminated)",   // 字符串没有结束引号
        R"({"a":1}trailing)", // 结尾有多余内容
        R"()",                // 空输入
    };

    for (const char* bad : bad_cases) {
        bool threw = false;
        try {
            iotd::json::Parse(bad);
        } catch (const iotd::json::ParseError& e) {
            threw = true;
            // 错误信息里必须带位置，否则排障时没用
            CHECK(e.position() <= std::string(bad).size());
        } catch (...) {
            threw = true; // 抛了别的异常也算"没崩溃"，但下面会单独检查类型
        }
        CHECK(threw);
    }
}

void TestDeviceClientRejectsBadData() {
    std::printf("[测试] DeviceClient 对业务坏数据的处理\n");

    iotd::DeviceMessage msg;
    std::string err;

    // 缺 device_id
    CHECK(!iotd::DeviceClient::ParseFrame(R"({"temp":25.0})", msg, err));

    // 顶层不是对象
    CHECK(!iotd::DeviceClient::ParseFrame("[1,2,3]", msg, err));

    // 温度超范围（模拟传感器故障）
    CHECK(
        !iotd::DeviceClient::ParseFrame(R"({"device_id":"d1","temp":500.0,"humid":50})", msg, err));

    // 湿度超范围
    CHECK(
        !iotd::DeviceClient::ParseFrame(R"({"device_id":"d1","temp":25.0,"humid":150})", msg, err));

    // 空帧（串口空闲时可能出现）
    CHECK(!iotd::DeviceClient::ParseFrame("", msg, err));

    // 合法数据必须通过（防止前面的判断写得太严把好数据也挡了）
    CHECK(iotd::DeviceClient::ParseFrame(R"({"device_id":"d1","temp":25.0,"humid":50})", msg, err));
}

void TestDumpRoundTrip() {
    std::printf("[测试] 序列化往返（Dump 后再 Parse 结果要一致）\n");

    const std::string original = R"({"a":1.500,"b":"text","c":true})";
    const auto first = iotd::json::Parse(original);
    const std::string dumped = iotd::json::Dump(first);
    const auto second = iotd::json::Parse(dumped);

    CHECK(iotd::json::GetString(second, "b") == "text");
    CHECK(NearlyEqual(iotd::json::GetNumber(second, "a"), 1.5));
    CHECK(iotd::json::GetString(first, "missing", "default") == "default");
    CHECK(NearlyEqual(iotd::json::GetNumber(first, "missing", -1.0), -1.0));
}

} // namespace

int main() {
    std::printf("===== iotd 测试开始 =====\n");

    TestJsonBasics();
    TestEscapeSequences();
    TestParseErrors();
    TestValidMessage();
    TestDeviceClientRejectsBadData();
    TestDumpRoundTrip();

    std::printf("===== 测试结束：通过 %d，失败 %d =====\n", g_passed, g_failed);

    // 返回非 0 表示测试失败
    // 这个约定让 CI（持续集成）能自动判断代码是否合格 ——
    // 这就是"自动化测试"和"手动点一遍"的本质区别
    return g_failed == 0 ? 0 : 1;
}
