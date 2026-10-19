// ===========================================================================
// src/line_framer.cpp
// ===========================================================================

#include "iotd/line_framer.h"

namespace iotd {

LineFramer::LineFramer(std::size_t max_buffer_bytes)
    // 传 0 会让每次 Append 之后立刻判定超限，等于永远收不到一帧；抬到 1 免得调用方踩坑
    : max_buffer_bytes_(max_buffer_bytes == 0 ? 1 : max_buffer_bytes) {}

void LineFramer::Append(const char* data, std::size_t n) {
    if (data == nullptr || n == 0) {
        return;
    }
    if (tap_) {
        tap_(data, n);
    }
    buffer_.append(data, n);
}

std::optional<std::string> LineFramer::Next() {
    const std::size_t newline_pos = buffer_.find('\n');
    if (newline_pos == std::string::npos) {
        // 一直等不到换行符，缓冲区又超过上限：整块丢掉，别让它无限长下去
        // 判定放在这里而不是 Append 里，是因为要等"这一批字节处理完"才知道还缺不缺换行
        if (buffer_.size() > max_buffer_bytes_) {
            dropped_bytes_ += buffer_.size();
            buffer_.clear();
        }
        return std::nullopt;
    }

    std::string frame = buffer_.substr(0, newline_pos);
    buffer_.erase(0, newline_pos + 1);

    // 设备端用 \r\n 收尾时 \r 会留在帧尾，JSON 解析器不认识它，会在帧尾报非法字符
    if (!frame.empty() && frame.back() == '\r') {
        frame.pop_back();
    }
    return frame;
}

void LineFramer::Reset() {
    buffer_.clear();
}

} // namespace iotd
