// ===========================================================================
// iotd/capture.h
//
// 录制文件的读写，录制器（CaptureRecorder）和回放器（FileReplaySource）共用这一份格式
//
// 一行一条记录，'#' 开头是注释，空行跳过：
//
//     @毫秒<TAB>字节块
//
// 毫秒是相对本段录制开始的偏移，字节块是那一刻从串口真正收到的原始字节
// 记的是"每次收到多少字节"，不是切好的帧 —— 半包、粘包只有原始字节块才留得住
// ===========================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iotd {
namespace capture {

struct Chunk {
    std::int64_t at_ms = 0; // 相对录制开始的毫秒数
    std::string bytes;      // 原始字节，不是文本
};

// 把字节块转成能放进一行文件的内容：'\'、'\t'、'\r'、'\n' 和 0x20 以下的字节转义，
// 其余字节原样写出，这样中文内容在文件里仍然可读
std::string Escape(const char* data, std::size_t n);

// 解析一行（不含行尾换行符）
// 失败时 error 里是给人看的原因，不带文件名和行号，由调用方补
bool ParseLine(const std::string& line, Chunk& out, std::string& error);

// 读整个文件，按文件里出现的顺序放进 out
bool LoadFile(const std::string& path, std::vector<Chunk>& out, std::string& error);

} // namespace capture
} // namespace iotd
