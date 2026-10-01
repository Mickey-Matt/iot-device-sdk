// ===========================================================================
// src/mock_source.cpp
// ===========================================================================

#include "iotd/mock_source.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "iotd/logger.h"

namespace iotd {
namespace {

// 取当前 Unix 毫秒时间戳
std::int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

MockSource::MockSource(int interval_ms)
    : interval_ms_(interval_ms),
      // 用固定种子(42)：每次运行产生的"随机"数据都一样。
      // 这在调试时非常重要 —— 你能复现上一次看到的现象。
      // 如果每次数据都不同，你永远说不清"这个问题到底是不是随机的"。
      rng_(42),
      temp_dist_(24.0, 28.0),
      humid_dist_(50.0, 70.0) {}

bool MockSource::Open(std::string& error) {
    if (interval_ms_ <= 0) {
        error = "interval_ms 必须大于 0";
        return false;
    }
    opened_ = true;
    next_due_ms_ = NowMs() + interval_ms_;
    LOG_INFO("MockSource 已启动，每 " << interval_ms_ << " ms 产出一帧");
    return true;
}

void MockSource::Close() {
    opened_ = false;
}

std::string MockSource::BuildFrame() {
    ++counter_;

    if (inject_corrupted_) {
        inject_corrupted_ = false;
        // 故意缺一个右花括号，模拟"串口粘包/丢字节"造成的半包。
        // 真实项目里这种情况比你想象的常见得多。
        return R"({"device_id":"fish-tank-01","temp":25.1,"humid":60.2)";
    }

    // 偶尔注入一帧坏数据（每 7 帧一次），
    // 让你在正常运行时也能看到程序如何优雅处理坏报文，而不是崩溃。
    if (counter_ % 7 == 0) {
        return R"({"device_id":"fish-tank-01","temp":25.1,)";
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  R"({"device_id":"fish-tank-01","timestamp_ms":%lld,)"
                  R"("temp":%.2f,"humid":%.2f,"light_lux":%d})",
                  static_cast<long long>(NowMs()),
                  temp_dist_(rng_),
                  humid_dist_(rng_),
                  static_cast<int>(300 + counter_ % 50));
    return buf;
}

ReadResult MockSource::Poll() {
    ReadResult result;

    if (!opened_) {
        result.error = "MockSource 尚未打开";
        return result;
    }

    // 非阻塞语义：时间没到就直接返回"没数据"。
    // 这样主循环可以继续做别的事（比如检查用户有没有按 Ctrl+C）。
    const std::int64_t now = NowMs();
    if (now < next_due_ms_) {
        return result;  // has_data 保持 false
    }
    next_due_ms_ = now + interval_ms_;

    result.has_data = true;
    result.frame = BuildFrame();
    return result;
}

}  // namespace iotd
