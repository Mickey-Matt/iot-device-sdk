// ===========================================================================
// src/device_client.cpp
// ===========================================================================

#include "iotd/device_client.h"

#include <chrono>

#include "iotd/json.h"
#include "iotd/logger.h"

namespace iotd {
namespace {

const std::int64_t kStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();

std::int64_t SteadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

DeviceClient::DeviceClient(IDataSource& source, TelemetryHandler handler)
    : source_(source), handler_(std::move(handler)) {}
// std::move(handler) 把传进来的回调"搬"进成员变量，避免一次拷贝
// 对于 std::function（内部可能持有 lambda 的捕获变量），这个开销不能忽略

bool DeviceClient::Start(std::string& error) {
    if (running_) {
        error = "DeviceClient 已经在运行中";
        return false;
    }
    if (!source_.Open(error)) {
        return false;
    }
    running_ = true;
    LOG_INFO("DeviceClient 已启动，数据源：" << source_.Name());
    return true;
}

void DeviceClient::Stop() {
    if (!running_) {
        return;
    }
    source_.Close();
    running_ = false;
    LOG_INFO("DeviceClient 已停止。统计：收到 "
             << stats_.frames_received << " 帧，成功 " << stats_.frames_ok << " 帧，解析失败 "
             << stats_.parse_failed << " 帧，数据源错误 " << stats_.source_errors << " 次");
}

bool DeviceClient::ParseFrame(const std::string& frame, DeviceMessage& out, std::string& error) {
    if (frame.empty()) {
        error = "空帧";
        return false;
    }

    try {
        const json::JsonValue root = json::Parse(frame);

        if (!root.IsObject()) {
            error = "报文顶层不是一个 JSON 对象";
            return false;
        }

        out.device_id = json::GetString(root, "device_id");
        if (out.device_id.empty()) {
            error = "报文缺少 device_id 字段";
            return false;
        }

        out.timestamp_ms = static_cast<std::int64_t>(
            json::GetNumber(root, "timestamp_ms", static_cast<double>(NowMs())));
        out.reading.temperature_c = json::GetNumber(root, "temp", 0.0);
        out.reading.humidity_pct = json::GetNumber(root, "humid", 0.0);
        out.reading.light_lux = static_cast<int>(json::GetNumber(root, "light_lux", 0.0));

        // 数据合理性校验：这一层经常被新手忽略，但它是"技术支持"的核心价值
        // 温度不可能是 500 度 —— 如果有，说明传感器坏了或报文被截断拼错了，
        // 这时候应该报警而不是把脏数据上报到云端
        if (out.reading.temperature_c < -40.0 || out.reading.temperature_c > 80.0) {
            error = "温度值超出合理范围：" + std::to_string(out.reading.temperature_c);
            return false;
        }
        if (out.reading.humidity_pct < 0.0 || out.reading.humidity_pct > 100.0) {
            error = "湿度值超出合理范围：" + std::to_string(out.reading.humidity_pct);
            return false;
        }
        return true;

    } catch (const json::ParseError& e) {
        // 关键设计：JSON 解析失败【不】让程序退出，只记录并跳过这一帧
        // 理由：现场环境中偶发的坏帧是常态（干扰、拔插、波特率抖动），
        //       如果一帧坏数据就让整个服务挂掉，那是不可接受的
        //       这个取舍在面试里非常值得讲
        error =
            std::string("JSON 解析失败（位置 ") + std::to_string(e.position()) + "）：" + e.what();
        return false;
    }
}

ProcessResult DeviceClient::PollOnce() {
    if (!running_) {
        return ProcessResult::NoData;
    }

    const ReadResult read = source_.Poll();

    // 数据源报错：记录并返回，由上层决定要不要重连
    if (!read.error.empty()) {
        ++stats_.source_errors;
        LOG_ERROR("数据源错误：" << read.error);
        return ProcessResult::SourceError;
    }

    if (!read.has_data) {
        return ProcessResult::NoData;
    }

    ++stats_.frames_received;
    stats_.last_frame_ms = NowMs();

    DeviceMessage message;
    std::string parse_error;
    if (!ParseFrame(read.frame, message, parse_error)) {
        ++stats_.parse_failed;
        // 把坏帧原样打出来 —— 排查时你最需要看到的就是"它到底长什么样"
        // 只打印"解析失败"而不打印内容，是新手最常犯的日志错误
        LOG_WARN("丢弃一帧坏数据：" << parse_error << " | 原始内容: " << read.frame);
        return ProcessResult::ParseFailed;
    }

    ++stats_.frames_ok;

    // 调用回调把数据交给上层，这里用 if (handler_) 判空 ——
    // 客户可能只想解析不想上报，允许回调为空
    if (handler_) {
        handler_(message);
    }
    return ProcessResult::Ok;
}

bool DeviceClient::PublishTelemetry(const SensorReading& reading) {
    DeviceMessage message;
    message.device_id = "fish-tank-01";
    message.timestamp_ms = NowMs();
    message.uptime_ms = SteadyNowMs() - kStartMs;
    message.sequence = ++sequence_;
    message.reading = reading;

    if (!handler_) {
        LOG_WARN("未设置上报回调，PublishTelemetry 被忽略");
        return false;
    }
    handler_(message);
    return true;
}

} // namespace iotd
