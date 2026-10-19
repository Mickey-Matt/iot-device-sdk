// ===========================================================================
// src/capture_recorder.cpp
// ===========================================================================

#include "iotd/capture_recorder.h"

#include <chrono>
#include <cstdio>
#include <utility>

#include "iotd/capture.h"
#include "iotd/logger.h"

namespace iotd {
namespace {

// 用单调时钟算相对时间：system_clock 会被 NTP 校时和手动改时间直接拽走，
// 一拽就会出现负的毫秒数或者跳跃，回放时序全乱
std::int64_t SteadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

CaptureRecorder::CaptureRecorder(std::string path, std::size_t max_segment_bytes, int max_segments)
    : base_path_(std::move(path)),
      // 段数至少为 1，否则换段时取模会除以 0
      max_segment_bytes_(max_segment_bytes == 0 ? 1024 : max_segment_bytes),
      max_segments_(max_segments < 1 ? 1 : max_segments) {}

CaptureRecorder::~CaptureRecorder() {
    Close();
}

bool CaptureRecorder::Open(std::string& error) {
    if (active_) {
        error = "录制已经在进行中";
        return false;
    }

    start_ms_ = SteadyNowMs();
    if (!OpenSegment(0, error)) {
        return false;
    }

    // 上一轮留下的旧段要清掉，否则回放会先读到上一轮的尾巴
    for (int i = 1; i < max_segments_; ++i) {
        const std::string stale = base_path_ + "." + std::to_string(i);
        std::remove(stale.c_str());
    }

    active_ = true;
    recorded_bytes_ = 0;
    lost_bytes_ = 0;
    return true;
}

void CaptureRecorder::Close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    active_ = false;
}

void CaptureRecorder::OnBytes(const char* data, std::size_t n) {
    if (!active_ || data == nullptr || n == 0) {
        return;
    }

    // 本轮写下去会超出单段上限就换段。段里还没有内容时不换，免得空段占掉一个名额
    if (segment_bytes_ > 0 && segment_bytes_ + n > max_segment_bytes_) {
        if (!Rotate()) {
            return;
        }
    }

    const std::int64_t at_ms = SteadyNowMs() - start_ms_;
    out_ << '@' << at_ms << '\t' << capture::Escape(data, n) << '\n';
    if (!out_) {
        // 磁盘满、目录被删、权限没了：停录制，别把数据链路一起拖下水
        active_ = false;
        lost_bytes_ += n;
        LOG_ERROR("录制写入失败，已停止录制：" << current_path_ << "（本块 " << n << " 字节未记录）");
        return;
    }

    segment_bytes_ += n;
    recorded_bytes_ += n;
}

bool CaptureRecorder::OpenSegment(int index, std::string& error) {
    const std::string path = base_path_ + "." + std::to_string(index);

    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        error = "无法写入录制文件：" + path;
        return false;
    }

    const std::string header = "# iotd 串口录制，一行一条：@毫秒<TAB>字节块\n";
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!out_) {
        error = "无法写入录制文件：" + path;
        out_.close();
        return false;
    }

    current_path_ = path;
    segment_ = index;
    segment_bytes_ = 0;
    // 每段都有自己的时间零点，这样单独回放某一段时不用等前面那几段的时间
    start_ms_ = SteadyNowMs();
    return true;
}

bool CaptureRecorder::Rotate() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }

    const int next = (segment_ + 1) % max_segments_;
    std::string error;
    if (!OpenSegment(next, error)) {
        active_ = false;
        LOG_ERROR("录制换段失败，已停止录制：" << error);
        return false;
    }
    LOG_INFO("录制换到下一段：" << current_path_);
    return true;
}

} // namespace iotd
