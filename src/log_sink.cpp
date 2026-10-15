// ===========================================================================
// src/log_sink.cpp
// ===========================================================================

#include "iotd/log_sink.h"

#include <utility>

#include "iotd/logger.h"

namespace iotd {

LogSink::LogSink(std::string tag) : tag_(std::move(tag)) {}

bool LogSink::Open(std::string& error) {
    // 打日志不需要"打开"，但接口要求有这一步
    // 返回 true 表示"这个出口随时可用"，不是敷衍 ——
    // 调用方不需要为不同出口写不同的启动代码，这正是接口的价值
    error.clear();
    LOG_INFO("LogSink 已就绪（只打日志，不联网）");
    return true;
}

void LogSink::Close() {}

bool LogSink::Publish(const DeviceMessage& message, std::string& error) {
    error.clear();
    LOG_INFO((tag_.empty() ? std::string() : "[" + tag_ + "] ")
             << "上报 device=" << message.device_id << " temp=" << message.reading.temperature_c
             << "C humid=" << message.reading.humidity_pct << "% light=" << message.reading.light_lux
             << "lux ts=" << message.timestamp_ms << " 开机" << message.uptime_ms << "ms");
    return true;
}

} // namespace iotd
