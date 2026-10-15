// ===========================================================================
// iotd/log_sink.h
//
// 一个"只打日志、不联网"的上报出口
//
// 它存在的理由：演示"出口可换"这件事
//   同一份 DeviceClient 代码，出口传 MqttSink 就上云，
//   传 LogSink 就只打在屏幕上 —— 业务代码一行不用改
//
// 和 DeviceClient 的上报回调（std::function）比起来，接口的好处是：
//   回调只能"改一个函数"，接口可以"换一整个实现"，
//   而且实现类可以带自己的状态（比如这里的 tag_ 前缀）
// ===========================================================================

#pragma once

#include <string>

#include "iotd/telemetry_sink.h"

namespace iotd {

class LogSink : public ITelemetrySink {
public:
    // tag 会作为日志前缀，用来区分多个出口（比如 "[mock]" / "[serial]"）
    // 不传就不加前缀
    explicit LogSink(std::string tag = "");

    bool Open(std::string& error) override;
    void Close() override;
    bool Publish(const DeviceMessage& message, std::string& error) override;

private:
    std::string tag_;
};

} // namespace iotd
