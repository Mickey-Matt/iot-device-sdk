// ===========================================================================
// iotd/line_framer.h
//
// 按换行符切帧：把字节流切成一条条完整的消息
//
// 一次读取拿到的内容长度是任意的，可能只有半条消息，也可能是三条拼在一起
// 分帧就是先把字节攒起来，等出现 '\n' 才交出一条消息
// ===========================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "iotd/framer.h"

namespace iotd {

class LineFramer : public IFramer {
public:
    // 用 std::function 而不是模板参数：调用方可能是录制器、可能是测试里的 lambda，
    // 类型不固定，放虚函数或模板会把接口污染得更厉害
    using Tap = std::function<void(const char* data, std::size_t n)>;

    // max_buffer_bytes：收不到 '\n' 时缓冲区允许长到多大，超过就把已攒的整块丢掉
    // 不设上限的话，波特率配错时收到的乱码永远等不到换行，内存会被一点点吃光
    //
    // half_frame_timeout_ms：一截半帧在缓冲区里待多久还没等到后续字节就丢掉
    // 调太短会把传输慢的正常帧当成半帧杀掉，调太长会让半帧和后来的数据拼成一条错帧
    explicit LineFramer(std::size_t max_buffer_bytes = 8 * 1024,
                        int half_frame_timeout_ms = 1000);

    void Append(const char* data, std::size_t n) override;
    std::optional<std::string> Next() override;
    void Reset() override;
    bool Tick() override;

    std::size_t BufferedBytes() const override { return buffer_.size(); }
    std::size_t MaxBufferBytes() const { return max_buffer_bytes_; }
    std::size_t OverflowBytes() const override { return overflow_bytes_; }
    std::size_t TimeoutBytes() const override { return timeout_bytes_; }

    // 每次 Append 都会把收到的原始字节原样回调一次，用来录制现场数据
    // 回调发生在入缓冲之前，所以录到的是串口上真实跑过的字节块，不是切好的帧
    // 传空函数对象即关闭
    void SetTap(Tap tap) { tap_ = std::move(tap); }

private:
    std::string buffer_;
    std::size_t max_buffer_bytes_;
    int half_frame_timeout_ms_;
    std::size_t overflow_bytes_ = 0;
    std::size_t timeout_bytes_ = 0;
    std::int64_t last_append_ms_ = 0;
    Tap tap_;
};

} // namespace iotd
