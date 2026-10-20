// ===========================================================================
// iotd/framer.h
//
// 分帧接口：把字节流切成一条条完整的消息
//
// 数据源只负责搬字节，怎么切由实现决定（本项目按换行符切，见 line_framer.h）
// ===========================================================================

#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace iotd {

class IFramer {
public:
    virtual ~IFramer() = default;

    // 追加刚收到的字节，半条消息、三条消息拼在一起都能吃
    virtual void Append(const char* data, std::size_t n) = 0;

    // 取下一帧
    // 返回 nullopt 表示还没凑够一帧；返回空串表示收到了一个空行，那也是一帧
    virtual std::optional<std::string> Next() = 0;

    // 丢掉未成帧的字节，数据源重开时调用
    virtual void Reset() = 0;

    // 半帧超时检查，驱动循环里每轮调一次
    // 返回 true 表示这次真的把一截等太久的半帧丢掉了
    // 【注意】这个检查不能只在"读到新字节"时做：设备发了一半就没了的时候，
    //        后面的循环一个字节都读不到，半帧会一直躺在缓冲区里
    virtual bool Tick() = 0;

    virtual std::size_t BufferedBytes() const = 0;

    // 缓冲区超过上限被整块丢弃的累计字节数
    virtual std::size_t OverflowBytes() const = 0;

    // 半帧等太久被丢弃的累计字节数
    virtual std::size_t TimeoutBytes() const = 0;
};

} // namespace iotd
