// ===========================================================================
// src/capture.cpp
// ===========================================================================

#include "iotd/capture.h"

#include <fstream>
#include <limits>
#include <utility>

namespace iotd {
namespace capture {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

bool HexValue(char c, unsigned int& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<unsigned int>(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<unsigned int>(c - 'A') + 10;
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<unsigned int>(c - 'a') + 10;
        return true;
    }
    return false;
}

// 解开转义。反斜杠后面只认 \\ \t \r \n \xNN，别的都算坏文件
bool Unescape(const std::string& text, std::string& out, std::string& error) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 1 >= text.size()) {
            error = "行尾有一个孤立的反斜杠";
            return false;
        }
        const char next = text[++i];
        switch (next) {
            case '\\': out.push_back('\\'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'n': out.push_back('\n'); break;
            case 'x': {
                unsigned int hi = 0;
                unsigned int lo = 0;
                const bool has_hex = i + 2 < text.size() && HexValue(text[i + 1], hi) &&
                                     HexValue(text[i + 2], lo);
                if (!has_hex) {
                    error = "\\x 后面要跟两位十六进制数";
                    return false;
                }
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                break;
            }
            default: error = std::string("不认识的转义 \\") + next; return false;
        }
    }
    return true;
}

} // namespace

std::string Escape(const char* data, std::size_t n) {
    std::string out;
    if (data == nullptr) {
        return out;
    }
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const char raw = data[i];
        switch (raw) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            default: {
                const unsigned char byte = static_cast<unsigned char>(raw);
                if (byte < 0x20) {
                    out += "\\x";
                    out += kHexDigits[byte >> 4];
                    out += kHexDigits[byte & 0x0F];
                } else {
                    out += raw;
                }
                break;
            }
        }
    }
    return out;
}

bool ParseLine(const std::string& line, Chunk& out, std::string& error) {
    if (line.empty()) {
        error = "空行";
        return false;
    }
    if (line[0] != '@') {
        error = "行首不是 '@'";
        return false;
    }

    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        error = "毫秒数和字节块之间少一个 TAB";
        return false;
    }

    std::int64_t at_ms = 0;
    for (std::size_t i = 1; i < tab; ++i) {
        const char c = line[i];
        if (c < '0' || c > '9') {
            error = "毫秒数里出现了非数字";
            return false;
        }
        // 先判溢出再乘，否则数字太长会变成负数，回放时永远等不到那一块
        if (at_ms > (std::numeric_limits<std::int64_t>::max() - 9) / 10) {
            error = "毫秒数太大";
            return false;
        }
        at_ms = at_ms * 10 + (c - '0');
    }

    out.at_ms = at_ms;
    return Unescape(line.substr(tab + 1), out.bytes, error);
}

bool LoadFile(const std::string& path, std::vector<Chunk>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "打不开录制文件：" + path;
        return false;
    }

    out.clear();
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        // 文件被记事本或编辑器在 Windows 上改过时会是 CRLF，行尾的 \r 会把字节块末尾带脏
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        Chunk chunk;
        std::string line_error;
        if (!ParseLine(line, chunk, line_error)) {
            error = path + " 第 " + std::to_string(line_no) + " 行：" + line_error;
            return false;
        }
        out.push_back(std::move(chunk));
    }

    if (out.empty()) {
        error = "录制文件里没有任何记录：" + path;
        return false;
    }
    return true;
}

} // namespace capture
} // namespace iotd
