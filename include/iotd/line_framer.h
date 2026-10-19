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
#include <functional>
#include <optional>
#include <string>

namespace iotd {

class LineFramer {
public:
    // 用 std::function 而不是模板参数：调用方可能是录制器、可能是测试里的 lambda，
    // 类型不固定，放虚函数或模板会把接口污染得更厉害
    using Tap = std::function<void(const char* data, std::size_t n)>;

    // max_buffer_bytes：收不到 '\n' 时缓冲区允许长到多大，超过就把已攒的整块丢掉
    // 不设上限的话，波特率配错时收到的乱码永远等不到换行，内存会被一点点吃光
    explicit LineFramer(std::size_t max_buffer_bytes = 8 * 1024);

    // 追加刚收到的字节，可以是半条消息，也可以是三条消息拼在一起
    void Append(const char* data, std::size_t n);

    // 取下一帧，不含行尾的 '\n' 和 '\r'
    // 返回 nullopt 表示还没凑够一帧，等下次 Append
    // 返回空字符串表示收到了一个空行，那也是一帧，交给上层判定为坏数据
    std::optional<std::string> Next();

    // 丢掉缓冲区里未成帧的字节，串口重开时调用
    // DroppedBytes() 不清零，它是累计值，用来判断"到底丢过没有"
    void Reset();

    std::size_t BufferedBytes() const { return buffer_.size(); }
    std::size_t MaxBufferBytes() const { return max_buffer_bytes_; }
    std::size_t DroppedBytes() const { return dropped_bytes_; }

    // 每次 Append 都会把收到的原始字节原样回调一次，用来录制现场数据
    // 回调发生在入缓冲之前，所以录到的是串口上真实跑过的字节块，不是切好的帧
    // 传空函数对象即关闭
    void SetTap(Tap tap) { tap_ = std::move(tap); }

private:
    std::string buffer_;
    std::size_t max_buffer_bytes_;
    std::size_t dropped_bytes_ = 0;
    Tap tap_;
};

} // namespace iotd
