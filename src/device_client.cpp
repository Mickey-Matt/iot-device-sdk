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

DeviceClient::DeviceClient(IDataSource& source, IFramer& framer, ITelemetrySink& sink)
    : source_(source), framer_(framer), sink_(sink) {}
// 三个依赖都是引用，不拷贝也不接管所有权 ——
// 传进来的对象是栈上的局部变量还是堆上的，由调用方决定

bool DeviceClient::Start(std::string& error) {
    if (running_) {
        error = "DeviceClient 已经在运行中";
        return false;
    }
    if (!source_.Open(error)) {
        return false;
    }
    // 出口打不开就把数据源关回去，不要留下"开了一半"的状态
    // 这叫"失败回滚"，是多步初始化必须考虑的事
    if (!sink_.Open(error)) {
        source_.Close();
        return false;
    }
    // 上一轮残留的半帧不能带进新连接，否则新连接的第一帧会顶着一截旧字节
    framer_.Reset();
    running_ = true;
    LOG_INFO("DeviceClient 已启动，数据源：" << source_.Name());
    return true;
}

void DeviceClient::Stop() {
    if (!running_) {
        return;
    }
    // 先断出口再关数据源：数据源是"生产方"，停它之前先把下游封好，
    // 顺序反了的话，最后几帧会被送进一个已经关闭的出口
    sink_.Close();
    source_.Close();
    running_ = false;
    LOG_INFO("DeviceClient 已停止。统计：收到 "
             << stats_.frames_received << " 帧，成功 " << stats_.frames_ok
             << " 帧，解析失败 " << stats_.parse_failed << " 帧，上报失败 "
             << stats_.publish_failed << " 帧，传输错误 " << stats_.transport_errors
             << " 次，分帧丢弃 " << stats_.dropped_bytes << " 字节");
}

bool DeviceClient::ParseFrame(const std::string& frame, DeviceMessage& out,
                              std::string& error) {
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
        error = std::string("JSON 解析失败（位置 ") + std::to_string(e.position()) +
                "）：" + e.what();
        return false;
    }
}

ProcessResult DeviceClient::PollOnce() {
    if (!running_) {
        return ProcessResult::NoData;
    }

    // 分帧器的两个累计计数器取个快照，本轮结束后用差值判断"这一轮丢没丢、丢了多少"
    const std::size_t overflow_before = framer_.OverflowBytes();
    const std::size_t timeout_before = framer_.TimeoutBytes();

    // 半帧超时每轮都要查，不能等"读到新字节"再查：
    // 设备发了一半就没了的时候，后面的循环一个字节都读不到
    if (framer_.Tick()) {
        ++stats_.framer_timeout;
        LOG_WARN("半帧等待超时，丢弃 " << (framer_.TimeoutBytes() - timeout_before)
                                       << " 字节未成帧的数据");
    }

    ProcessResult result = ProcessResult::NoData;

    const ByteChunk chunk = source_.Poll();
    // 传输层报错：记录并返回，由上层决定要不要重连
    if (!chunk.error.empty()) {
        ++stats_.transport_errors;
        LOG_ERROR("数据源错误：" << chunk.error);
        result = ProcessResult::SourceError;
    } else {
        if (!chunk.bytes.empty()) {
            framer_.Append(chunk.bytes.data(), chunk.bytes.size());
            LOG_DEBUG("收到 " << chunk.bytes.size() << " 字节，分帧缓冲区现有 "
                              << framer_.BufferedBytes() << " 字节");
        }

        const std::optional<std::string> frame = framer_.Next();
        // 超限只会在"缓冲区里找不到换行符"的那一次 Next() 里发生，所以此刻必然没有帧可交
        if (framer_.OverflowBytes() != overflow_before) {
            ++stats_.framer_overflow;
            LOG_ERROR("缓冲区超过上限仍未收到换行符，丢弃 "
                      << (framer_.OverflowBytes() - overflow_before)
                      << " 字节（可能是波特率不匹配导致乱码）");
        }
        if (frame) {
            result = HandleFrame(*frame);
        }
    }

    stats_.dropped_bytes += (framer_.OverflowBytes() - overflow_before) +
                            (framer_.TimeoutBytes() - timeout_before);
    return result;
}

ProcessResult DeviceClient::HandleFrame(const std::string& frame) {
    ++stats_.frames_received;
    stats_.last_frame_ms = NowMs();

    DeviceMessage message;
    std::string parse_error;
    if (!ParseFrame(frame, message, parse_error)) {
        ++stats_.parse_failed;
        // 把坏帧原样打出来 —— 排查时你最需要看到的就是"它到底长什么样"
        // 只打印"解析失败"而不打印内容，是新手最常犯的日志错误
        LOG_WARN("丢弃一帧坏数据：" << parse_error << " | 原始内容: " << frame);
        return ProcessResult::ParseFailed;
    }

    ++stats_.frames_ok;

    // --- 贴标签 ---
    // 这两项是"每条消息各不相同"的属性，数据源产出的报文里【没有】，
    // 所以只能在这里补上：
    //   uptime_ms 由发送方算（本进程开机多久），
    //   sequence  由发送方数（这是发出的第几条）
    // 为什么不在 ParseFrame 里补？因为 ParseFrame 处理的是【别人发来的】报文，
    // 它的时间戳应该以报文里写的为准，不能拿本机的时间去覆盖
    message.uptime_ms = SteadyNowMs() - kStartMs;
    message.sequence = ++sequence_;

    // --- 交给出口 ---
    // 上报失败不让程序退出，只记一条警告 ——
    // 和设备坏帧同一个思路：现场断网是常态，不能因为发不出去就挂掉
    std::string publish_error;
    if (!sink_.Publish(message, publish_error)) {
        ++stats_.publish_failed;
        LOG_WARN("上报失败（第 " << message.sequence << " 条）：" << publish_error);
        return ProcessResult::PublishFailed;
    }
    return ProcessResult::Ok;
}

} // namespace iotd
