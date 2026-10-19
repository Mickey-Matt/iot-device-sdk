// ===========================================================================
// src/file_replay_source.cpp
// ===========================================================================

#include "iotd/file_replay_source.h"

#include <chrono>
#include <utility>

#include "iotd/logger.h"

namespace iotd {
namespace {

// 最后一块喂完后再等这么久，给"结尾是个半帧"的录制一个自然收尾的机会
constexpr std::int64_t kDrainMs = 300;

std::int64_t SteadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

FileReplaySource::FileReplaySource(std::string path) : path_(std::move(path)) {}

bool FileReplaySource::Open(std::string& error) {
    if (!capture::LoadFile(path_, chunks_, error)) {
        return false;
    }
    framer_.Reset();
    next_chunk_ = 0;
    start_ms_ = SteadyNowMs();
    last_feed_ms_ = start_ms_;
    opened_ = true;

    std::size_t total_bytes = 0;
    for (const auto& chunk : chunks_) {
        total_bytes += chunk.bytes.size();
    }
    LOG_INFO("回放文件已载入：" << path_ << "，" << chunks_.size() << " 块，共 " << total_bytes
                                << " 字节");
    return true;
}

void FileReplaySource::Close() {
    opened_ = false;
}

ReadResult FileReplaySource::Poll() {
    ReadResult result;
    if (!opened_) {
        result.error = "回放文件尚未打开";
        return result;
    }

    const std::int64_t elapsed = SteadyNowMs() - start_ms_;

    // 时间没到的块一块都不喂，两次读取之间的间隔也要照原样重现
    while (next_chunk_ < chunks_.size() && elapsed >= chunks_[next_chunk_].at_ms) {
        const capture::Chunk& chunk = chunks_[next_chunk_];
        framer_.Append(chunk.bytes.data(), chunk.bytes.size());
        last_feed_ms_ = SteadyNowMs();
        ++next_chunk_;
    }

    // 超限判定发生在 Next() 里，所以要把计数器的前后差当成检测手段：
    // 差值不为 0 说明这一轮把缓冲区整块丢了，而此时必然没有帧可交
    const std::size_t dropped_before = framer_.DroppedBytes();
    const std::optional<std::string> frame = framer_.Next();
    if (framer_.DroppedBytes() != dropped_before) {
        result.error = "缓冲区超过 " + std::to_string(framer_.MaxBufferBytes()) +
                       " 字节仍未收到换行符，已丢弃（可能是波特率不匹配导致乱码）";
        return result;
    }

    if (frame) {
        result.has_data = true;
        result.frame = std::move(*frame);
    }
    return result;
}

std::string FileReplaySource::Name() const {
    return "FileReplay(" + path_ + ")";
}

bool FileReplaySource::Finished() const {
    if (next_chunk_ < chunks_.size()) {
        return false;
    }
    if (framer_.BufferedBytes() == 0) {
        return true;
    }
    return SteadyNowMs() - last_feed_ms_ >= kDrainMs;
}

} // namespace iotd
