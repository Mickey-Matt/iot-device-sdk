// ===========================================================================
// iotd/json.h
//
// 手写一个"够用"的 JSON 解析器，为什么不用 nlohmann/json？
//   - 教学目的：你要能看懂每一行，而不是把别人的库当黑盒调用
//   - 零依赖：你现在连 CMake 都没跑通，不该再引入下载库的步骤
//   - 面试价值：手写解析器能讲的东西（递归下降、错误定位、类型系统）
//     比"我会用 nlohmann"多得多
//
// 支持的 JSON 子集（覆盖设备报文 99% 的场景）：
//   { "key": "字符串", "n": 12.5, "b": true, "o": { 嵌套对象 }, "a": [1,2] }
//   不支持：\uXXXX 转义、科学计数法 e、null 的语义化处理
//
// 【重要】这个文件是"接口声明"，具体实现在 src/json.cpp 里
// 这种"声明与实现分离"是 C++ 的基本工程习惯：
//   - 使用者只 include 头文件，不必关心实现
//   - 改实现不会引发大规模重新编译
// ===========================================================================

#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace iotd {
namespace json {

// ---------------------------------------------------------------------------
// JsonValue：一个 JSON 值
//
// C++ 是静态类型语言，一个变量只能是一种类型
// 但 JSON 里一个 value 可能是字符串、可能是数字、也可能是对象
// 怎么办？用 std::variant —— "这些类型里的一种"
//
// std::variant<std::string, double, bool, Object, Array>
//   意思是：它要么是 string，要么是 double，要么是 bool，要么是 Object……
// 这是 C++17 提供的"类型安全的 union"，比老的 union 安全得多
// ---------------------------------------------------------------------------

// 前向声明：Object 里要装 JsonValue，JsonValue 里又要装 Object，
// 互相引用，所以必须先声明名字、后补全定义
struct JsonValue;

using Object = std::map<std::string, JsonValue>;
// std::map 是"有序的键值对表"，为什么用 map 而不是 unordered_map？
// 因为 map 的 key 有序，打印 JSON 时字段顺序稳定，调试时更好看
// 面试点：map 是红黑树，查找 O(log n)；unordered_map 是哈希表，平均 O(1)
// 数据量小时两者没差别，稳定性更重要

using Array = std::vector<JsonValue>;

struct JsonValue {
    // 用一个枚举标记"当前装的是哪种类型"
    // 访问时必须先问类型，再取值 —— 这是类型安全的核心
    enum class Type { Null, Bool, Number, String, Object, Array };

    Type type = Type::Null;

    // 用 variant 同时容纳所有可能，只有 type 标记的那一个是有意义的
    std::variant<std::monostate, bool, double, std::string, Object, Array> data;

    // --- 构造函数：让 JsonValue 用起来像普通变量 ---
    JsonValue() = default;
    JsonValue(bool v) : type(Type::Bool), data(v) {}
    JsonValue(double v) : type(Type::Number), data(v) {}
    JsonValue(int v) : type(Type::Number), data(static_cast<double>(v)) {}
    JsonValue(const char* v) : type(Type::String), data(std::string(v)) {}
    JsonValue(std::string v) : type(Type::String), data(std::move(v)) {}
    JsonValue(Object v) : type(Type::Object), data(std::move(v)) {}
    JsonValue(Array v) : type(Type::Array), data(std::move(v)) {}

    // std::move 的作用：把 v 内部的数据"搬走"而不是"复制一份"
    // 对于 map/string 这类在堆上分配内存的类型，复制代价很高，
    // move 只是转移指针，几乎零成本，这是 C++11 最重要的性能特性之一

    // --- 类型判断：让调用方写出安全且好读的代码 ---
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }

    // --- 取值：类型不对就返回默认值，绝不抛异常 ---
    // 这是"库"和"玩具代码"的重要区别：
    // 库要考虑调用方拿到意外数据时不该崩溃
    std::string AsString(const std::string& fallback = "") const;
    double AsNumber(double fallback = 0.0) const;
    bool AsBool(bool fallback = false) const;
};

// ---------------------------------------------------------------------------
// ParseError：解析失败时抛出的异常，带出错位置
// 一个不告诉你"错在第几个字符"的解析器，在排障时等于没有
// ---------------------------------------------------------------------------
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& message, std::size_t position)
        : std::runtime_error(message), position_(position) {}

    std::size_t position() const { return position_; }

private:
    std::size_t position_;
};

// ---------------------------------------------------------------------------
// 对外主函数：把一段文本解析成 JsonValue
//   成功 → 返回 JsonValue（type == Object）
//   失败 → 抛 ParseError（内含出错字符位置）
// ---------------------------------------------------------------------------
JsonValue Parse(const std::string& text);

// 反向操作：把 JsonValue 变回 JSON 文本（上报数据时要用）
std::string Dump(const JsonValue& value);

// 便捷取值：从对象里按 key 取字符串/数字，取不到就给默认值
std::string GetString(const JsonValue& obj, const std::string& key,
                      const std::string& fallback = "");
double GetNumber(const JsonValue& obj, const std::string& key, double fallback = 0.0);

} // namespace json
} // namespace iotd
