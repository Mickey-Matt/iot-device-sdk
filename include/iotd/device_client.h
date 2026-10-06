// ===========================================================================
// iotd/device_client.h
//
// 【SDK 的对外门面】
//
// 如果这个项目是一个商品，那么 device_client.h 就是商品的说明书封面
// 使用者（客户）只需要看这个文件，就知道怎么用你的 SDK
//
// 设计原则（面试时照着讲）：
//   1. 构造时就明确依赖：传进来一个 IDataSource，而不是在内部 new 一个
//      这叫"依赖注入"，好处是客户可以用你的 SDK 接任何数据源
//   2. 不拥有数据源的所有权：只借用，不负责 delete
//      用裸引用 IDataSource& 而不是 shared_ptr，因为所有权不属于我
//   3. 回调用 std::function：客户可以传普通函数、lambda、成员函数绑定
//   4. 析构自动清理：RAII
//   5. 提供统计信息：上线后靠它判断链路是否健康
// ===========================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "iotd/data_source.h"
#include "iotd/types.h"

namespace iotd {

// 处理结果枚举，为什么不用 bool？
// 因为"失败"分很多种：解析错误可以跳过继续，
// 而"数据源断开"需要触发重连，用枚举能让调用方区分处理
enum class ProcessResult {
    Ok,          // 成功处理一帧
    NoData,      // 这次没读到数据（正常）
    ParseFailed, // 读到了数据但 JSON 不合法（可跳过，不致命）
    SourceError, // 数据源本身出错了（需要上层介入）
};

class DeviceClient {
public:
    // 上报回调：DeviceClient 解析出数据后调用它
    // 第一阶段打印到屏幕；第二阶段换成发 MQTT；第三阶段可能写数据库
    // —— 这一行 typedef 就是"未来接入 MQTT 不用改核心逻辑"的原因
    using TelemetryHandler = std::function<void(const DeviceMessage&)>;

    // 构造：传入数据源和上报回调
    //   注意 IDataSource& 是引用：明确表达"我不拥有它，也不负责销毁它"
    DeviceClient(IDataSource& source, TelemetryHandler handler);

    // 禁止拷贝，为什么？
    //   拷贝一个 DeviceClient 意味着两个对象共享同一个数据源和统计计数，
    //   这在逻辑上是错的，C++11 的 = delete 让编译器帮你在编译期拦住错误用法
    //   （面试题：这叫"Rule of Three/Five"的应用）
    DeviceClient(const DeviceClient&) = delete;
    DeviceClient& operator=(const DeviceClient&) = delete;

    ~DeviceClient() = default;

    // 启动/停止
    bool Start(std::string& error);
    void Stop();

    // 驱动一次：读一帧、解析、回调上报
    // 为什么不让 DeviceClient 自己开线程循环？
    //   因为"谁来驱动循环"是使用者的自由：可以 while 循环、可以用定时器、
    //   也可以放进别人的事件循环里，SDK 不该替客户决定调度方式
    //   —— 这是很值得在面试里讲的一个设计取舍
    ProcessResult PollOnce();

    // 主动上报一条数据（供测试和示例使用）
    bool PublishTelemetry(const SensorReading& reading);

    // --- 统计信息：观察链路健康度 ---
    // 面试时这样讲："上线后客户说设备老掉线，我先看统计：
    //                 如果是 parse_failed 涨得快，说明是串口参数或分帧问题；
    //                 如果是 frames_received 不涨，说明链路根本没通"
    struct Stats {
        std::uint64_t frames_received = 0; // 收到多少帧
        std::uint64_t frames_ok = 0;       // 解析成功多少帧
        std::uint64_t parse_failed = 0;    // 解析失败多少帧
        std::uint64_t source_errors = 0;   // 数据源错误次数
        std::int64_t last_frame_ms = 0;    // 最后一次收到数据的时间戳
    };
    const Stats& GetStats() const { return stats_; }

    // 把一帧文本解析成 DeviceMessage
    // 声明为 static，因为它不需要访问任何成员变量 ——
    // 这样测试时可以直接调 DeviceClient::ParseFrame(...) 而不用构造对象
    static bool ParseFrame(const std::string& frame, DeviceMessage& out, std::string& error);

private:
    IDataSource& source_;      // 借用，不拥有
    TelemetryHandler handler_; // 上报回调
    bool running_ = false;
    Stats stats_;
};

} // namespace iotd
