// ===========================================================================
// iotd/device_client.h
//
// 【SDK 的对外门面】
//
// 如果这个项目是一个商品，那么 device_client.h 就是商品的说明书封面
// 使用者（客户）只需要看这个文件，就知道怎么用你的 SDK
//
// 它干三件事：从数据源搬字节 → 分帧 → 解析校验贴标签，然后交给出口
//
//     数据源                  DeviceClient                       出口
//   IDataSource ──Poll()──▶  分帧 → 解析 → 校验 → 贴标签  ──Publish()──▶  ITelemetrySink
//   （从哪来）        ▲                                        （往哪去）
//                    │
//                IFramer（同一条分帧路径，串口和回放共用）
//
// 它自己【不碰网络】，也不知道出口那边是 MQTT 还是日志文件 ——
// 这就是"采集"和"传输"分开的意义：换任意一边，这边都不用改
//
// 设计原则（面试时照着讲）：
//   1. 构造时就明确依赖：传进来一个 IDataSource、一个 IFramer 和一个 ITelemetrySink，
//      而不是在内部 new —— 这叫"依赖注入"，三边都可以替换
//   2. 不拥有这三个对象的所有权：只借用，不负责 delete
//      用裸引用而不是 shared_ptr，因为所有权不属于我
//   3. 析构自动清理：RAII
//   4. 提供统计信息：上线后靠它判断链路是否健康
// ===========================================================================

#pragma once

#include <cstdint>
#include <string>

#include "iotd/data_source.h"
#include "iotd/framer.h"
#include "iotd/telemetry_sink.h"
#include "iotd/types.h"

namespace iotd {

// 处理结果枚举，为什么不用 bool？
// 因为"失败"分很多种：解析错误可以跳过继续，
// 而"数据源断开"需要触发重连，用枚举能让调用方区分处理
enum class ProcessResult {
    Ok,            // 成功处理一帧，并且已经交给出口
    NoData,        // 这次没读到数据（正常）
    ParseFailed,   // 读到了数据但 JSON 不合法（可跳过，不致命）
    PublishFailed, // 数据没问题，但出口那边发失败（通常是断网）
    SourceError,   // 传输层出错（串口读失败、文件打不开），需要上层介入
};

class DeviceClient {
public:
    // 构造：数据从哪来、怎么切帧、往哪去
    //   三个参数通常也是三个不同的对象，典型接法见 examples/mqtt_demo.cpp
    //
    //   分帧器单独传进来而不是在内部 new，是因为录制要挂在它上面：
    //   录到的必须是"分帧之前"的原始字节块，内部自己 new 一个，外面就够不着了
    //
    //   用引用而不是 shared_ptr：明确表达"我不拥有它们，也不负责销毁"
    DeviceClient(IDataSource& source, IFramer& framer, ITelemetrySink& sink);

    // 禁止拷贝，为什么？
    //   拷贝一个 DeviceClient 意味着两个对象共享同一个数据源和统计计数，
    //   这在逻辑上是错的，C++11 的 = delete 让编译器帮你在编译期拦住错误用法
    //   （面试题：这叫"Rule of Three/Five"的应用）
    DeviceClient(const DeviceClient&) = delete;
    DeviceClient& operator=(const DeviceClient&) = delete;

    ~DeviceClient() = default;

    // 启动/停止：数据源和出口一起开关，任何一边失败都会回滚
    bool Start(std::string& error);
    void Stop();

    // 驱动一次：读一批字节 → 分帧 → 解析 → 校验 → 贴标签 → 交给出口
    //
    // 一次调用最多交出一帧，所以调用间隔别太长，否则积压的帧要排很久
    // 半帧超时也是在这里推进的，循环卡住不调，半帧就不会被判超时
    //
    // 为什么不让 DeviceClient 自己开线程循环？
    //   因为"谁来驱动循环"是使用者的自由：可以 while 循环、可以用定时器、
    //   也可以放进别人的事件循环里，SDK 不该替客户决定调度方式
    //   —— 这是很值得在面试里讲的一个设计取舍
    //   （Azure IoT SDK 的 IoTHubDeviceClient_LL_DoWork 是同一个思路）
    ProcessResult PollOnce();

    // --- 统计信息：观察链路健康度 ---
    // 面试时这样讲："上线后客户说设备老掉线，我先看统计：
    //                 如果是 parse_failed 涨得快，说明是串口参数或分帧问题；
    //                 如果是 frames_received 不涨，说明链路根本没通"
    struct Stats {
        std::uint64_t frames_received = 0;  // 收到多少帧
        std::uint64_t frames_ok = 0;        // 解析成功多少帧
        std::uint64_t parse_failed = 0;     // 解析失败多少帧
        std::uint64_t publish_failed = 0;   // 上报失败多少帧
        std::uint64_t transport_errors = 0; // 传输层错误次数（串口读失败、文件打不开）
        std::uint64_t framer_overflow = 0;  // 缓冲区超限被整块丢弃的次数
        std::uint64_t framer_timeout = 0;   // 半帧等太久被丢弃的次数
        std::uint64_t dropped_bytes = 0;    // 上面两种原因丢掉的字节总数
        std::int64_t last_frame_ms = 0;     // 最后一次收到数据的时间戳
    };
    const Stats& GetStats() const { return stats_; }

    // 把一帧文本解析成 DeviceMessage
    // 声明为 static，因为它不需要访问任何成员变量 ——
    // 这样测试时可以直接调 DeviceClient::ParseFrame(...) 而不用构造对象
    static bool ParseFrame(const std::string& frame, DeviceMessage& out,
                           std::string& error);

private:
    // 一帧已经切出来了，往下走：解析 → 校验 → 贴标签 → 交给出口
    ProcessResult HandleFrame(const std::string& frame);

    IDataSource& source_;  // 从哪读（借用，不拥有）
    IFramer& framer_;      // 怎么切帧（借用，不拥有）
    ITelemetrySink& sink_; // 往哪发（借用，不拥有）
    bool running_ = false;
    Stats stats_;
    std::uint64_t sequence_ = 0; // 已上报条数，用作报文序号
};

} // namespace iotd
