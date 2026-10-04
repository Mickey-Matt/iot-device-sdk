// ===========================================================================
// src/json.cpp  —— JSON 解析器的实现
//
// 解析方法叫"递归下降"（recursive descent）。思想极其简单：
//   从左到右扫描字符，遇到 { 就进入对象，遇到 [ 就进入数组，
//   对象/数组里再遇到 { 就再递归进去。
//
// 之所以能用递归，是因为 JSON 的语法本身就是递归定义的：
//   值 := 字符串 | 数字 | 布尔 | 对象 | 数组
//   对象 := { 键值对列表 }
//   值 := ……  又回到开头
//
// 这是计算机科学里最优雅的一类算法：数据结构是递归的，处理它的代码也是递归的。
// ===========================================================================

#include "iotd/json.h"

#include <cctype>  // std::isspace, std::isdigit
#include <cstdio>  // snprintf
#include <cstdlib> // std::strtod

namespace iotd {
namespace json {

// ---------------------------------------------------------------------------
// JsonValue 的三个取值函数
// ---------------------------------------------------------------------------
std::string JsonValue::AsString(const std::string& fallback) const {
    // std::get_if<T>(&data) 的语义：
    //   如果 data 里装的正好是 T，返回指向它的指针；
    //   否则返回 nullptr。绝不抛异常，所以要先判断。
    if (const auto* p = std::get_if<std::string>(&data)) {
        return *p;
    }
    return fallback;
}

double JsonValue::AsNumber(double fallback) const {
    if (const auto* p = std::get_if<double>(&data)) {
        return *p;
    }
    return fallback;
}

bool JsonValue::AsBool(bool fallback) const {
    if (const auto* p = std::get_if<bool>(&data)) {
        return *p;
    }
    return fallback;
}

// ===========================================================================
// 下面是解析器的内部实现。放进 detail 命名空间，
// 表示"这是内部细节，外部不要直接调用"。
// ===========================================================================
namespace detail {

// Parser 是一个"解析器对象"。
// 它内部持有两个状态：
//   text_ —— 要解析的原始文本（const 引用，不拷贝，零成本）
//   pos_  —— 当前扫描到第几个字符
// 这种"把状态和数据绑在一起"的做法，就是面向对象的基本功。
class Parser {
public:
    explicit Parser(const std::string& text) : text_(text), pos_(0) {}

    // 解析入口：解析一个值，然后要求后面除了空白不能再有别的东西
    JsonValue ParseDocument() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (pos_ != text_.size()) {
            Fail("解析完成后仍有多余字符");
        }
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_;

    // --- 基础工具 ---------------------------------------------------------

    // 还有字符可读吗？
    bool HasMore() const { return pos_ < text_.size(); }

    // 看一眼当前字符，但不移动位置
    char Peek() const { return HasMore() ? text_[pos_] : '\0'; }

    // 跳过空格、制表符、换行
    void SkipWhitespace() {
        while (HasMore() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        // 注意 static_cast<unsigned char>：
        // std::isspace 的参数是 int，如果直接传 char，
        // 当字符值 > 127（比如中文 UTF-8 字节）时会被解释成负数，行为未定义。
        // 这是 C++ 里极其经典的一个坑，面试也常问。
    }

    // 抛出一个"带位置信息"的错误
    [[noreturn]] void Fail(const std::string& message) const { throw ParseError(message, pos_); }
    // [[noreturn]] 是 C++11 属性，告诉编译器"这个函数一定不会正常返回"。
    // 好处：编译器知道后面的代码不会执行，不会误报"缺少返回值"的警告。

    // 期望读到某个字符，读不到就报错
    void Expect(char expected) {
        if (!HasMore() || text_[pos_] != expected) {
            Fail(std::string("期望字符 '") + expected + "'");
        }
        ++pos_;
    }

    // 匹配一个关键字，比如 "true"
    bool MatchLiteral(const char* literal) {
        std::size_t len = std::char_traits<char>::length(literal);
        if (text_.compare(pos_, len, literal) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    // --- 核心递归函数 -----------------------------------------------------

    // 解析任意一个值：先看第一个字符是什么，据此决定走哪个分支
    JsonValue ParseValue() {
        if (!HasMore()) {
            Fail("输入意外结束");
        }

        switch (Peek()) {
        case '{': return ParseObject();
        case '[': return ParseArray();
        case '"': return JsonValue(ParseString());
        case 't':
            if (MatchLiteral("true"))
                return JsonValue(true);
            Fail("无法识别的字面量");
        case 'f':
            if (MatchLiteral("false"))
                return JsonValue(false);
            Fail("无法识别的字面量");
        case 'n':
            if (MatchLiteral("null"))
                return JsonValue(); // 默认构造即 Null
            Fail("无法识别的字面量");
        default:
            // 剩下只可能是数字
            return JsonValue(ParseNumber());
        }
    }

    // 解析对象：{ "k": v, "k2": v2 }
    JsonValue ParseObject() {
        Expect('{');
        Object result;
        SkipWhitespace();

        // 空对象 {} 的情况
        if (Peek() == '}') {
            ++pos_;
            return JsonValue(std::move(result));
        }

        while (true) {
            SkipWhitespace();
            if (Peek() != '"') {
                Fail("对象的键必须是字符串");
            }
            std::string key = ParseString();

            SkipWhitespace();
            Expect(':'); // 键和值之间必须有冒号
            SkipWhitespace();

            result[key] = ParseValue(); // 递归解析值

            SkipWhitespace();
            if (Peek() == ',') {
                ++pos_;
                continue; // 还有下一个键值对
            }
            if (Peek() == '}') {
                ++pos_;
                break; // 对象结束
            }
            Fail("对象中期望 ',' 或 '}'");
        }
        return JsonValue(std::move(result));
    }

    // 解析数组：[1, 2, 3]
    JsonValue ParseArray() {
        Expect('[');
        Array result;
        SkipWhitespace();

        if (Peek() == ']') {
            ++pos_;
            return JsonValue(std::move(result));
        }

        while (true) {
            SkipWhitespace();
            result.push_back(ParseValue());

            SkipWhitespace();
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == ']') {
                ++pos_;
                break;
            }
            Fail("数组中期望 ',' 或 ']'");
        }
        return JsonValue(std::move(result));
    }

    // 解析字符串：从开引号读到最后那个未转义的引号
    std::string ParseString() {
        Expect('"');
        std::string out;

        while (true) {
            if (!HasMore()) {
                Fail("字符串没有结束引号");
            }
            char c = text_[pos_++];

            if (c == '"') {
                break; // 结束了
            }

            if (c == '\\') {
                // 转义字符：\" \\ \/ \n \r \t
                if (!HasMore()) {
                    Fail("转义符后缺少字符");
                }
                char esc = text_[pos_++];
                switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: Fail("不支持的转义字符");
                }
                continue;
            }

            out.push_back(c);
        }
        return out;
    }

    // 解析数字：支持负号、整数、小数；不支持科学计数法
    double ParseNumber() {
        std::size_t start = pos_;

        if (Peek() == '-') {
            ++pos_;
        }
        while (HasMore() && std::isdigit(static_cast<unsigned char>(Peek()))) {
            ++pos_;
        }
        if (Peek() == '.') {
            ++pos_;
            while (HasMore() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
            }
        }

        if (pos_ == start) {
            Fail("期望一个数字");
        }

        // 把刚才扫过的那段子串交给标准库转成 double
        const std::string token = text_.substr(start, pos_ - start);
        return std::strtod(token.c_str(), nullptr);
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// 对外的 Parse 与 Dump
// ---------------------------------------------------------------------------
JsonValue Parse(const std::string& text) {
    detail::Parser parser(text);
    return parser.ParseDocument();
}

namespace {
// 缩进工具：递归打印时每一层多两个空格
void Indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void DumpValue(const JsonValue& value, std::string& out, int depth) {
    switch (value.type) {
    case JsonValue::Type::Null: out += "null"; break;
    case JsonValue::Type::Bool: out += value.AsBool() ? "true" : "false"; break;
    case JsonValue::Type::Number: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", value.AsNumber());
        out += buf;
        break;
    }
    case JsonValue::Type::String:
        out += '"';
        for (char c : value.AsString()) {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            if (c == '\n') {
                out += "\\n";
                continue;
            }
            out.push_back(c);
        }
        out += '"';
        break;
    case JsonValue::Type::Object: {
        const auto& obj = std::get<Object>(value.data);
        if (obj.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        std::size_t i = 0;
        for (const auto& kv : obj) {
            Indent(out, depth + 1);
            out += '"' + kv.first + "\": ";
            DumpValue(kv.second, out, depth + 1);
            if (++i < obj.size())
                out += ',';
            out += '\n';
        }
        Indent(out, depth);
        out += '}';
        break;
    }
    case JsonValue::Type::Array: {
        const auto& arr = std::get<Array>(value.data);
        if (arr.empty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (std::size_t i = 0; i < arr.size(); ++i) {
            Indent(out, depth + 1);
            DumpValue(arr[i], out, depth + 1);
            if (i + 1 < arr.size())
                out += ',';
            out += '\n';
        }
        Indent(out, depth);
        out += ']';
        break;
    }
    }
}
} // namespace

std::string Dump(const JsonValue& value) {
    std::string out;
    DumpValue(value, out, 0);
    return out;
}

std::string GetString(const JsonValue& obj, const std::string& key, const std::string& fallback) {
    if (!obj.IsObject())
        return fallback;
    const auto& map = std::get<Object>(obj.data);
    auto it = map.find(key);
    if (it == map.end())
        return fallback;
    return it->second.AsString(fallback);
}

double GetNumber(const JsonValue& obj, const std::string& key, double fallback) {
    if (!obj.IsObject())
        return fallback;
    const auto& map = std::get<Object>(obj.data);
    auto it = map.find(key);
    if (it == map.end())
        return fallback;
    return it->second.AsNumber(fallback);
}

} // namespace json
} // namespace iotd
