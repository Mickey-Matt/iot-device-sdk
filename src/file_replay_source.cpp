// ===========================================================================
// src/file_replay_source.cpp
// ===========================================================================

#include "iotd/file_replay_source.h"

#include <chrono>
#include <utility>

#include "iotd/logger.h"

namespace iotd {
namespace {

// 最后一块喂完后再等这么久，让驱动循环把同一块里剩下的帧取完
// 每轮 PollOnce 只取一帧，20ms 一轮，500ms 够取二十几帧
constexpr std::int64_t kDrainMs = 500;

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
    next_chunk_ = 0;
    start_ms_ = SteadyNowMs();
    last_feed_ms_ = start_ms_;
    opened_ = true;

    std::size_t total_bytes = 0;
    for (const auto& chunk : chunks_) {
        total_bytes += chunk.bytes.size();
    }
    LOG_INFO("回放文件已载入：" << path_ << "，" << chunks_.size() << " 块，共 "
                                << total_bytes << " 字节");
    return true;
}

void FileReplaySource::Close() {
    opened_ = false;
}

ByteChunk FileReplaySource::Poll() {
    ByteChunk result;
    if (!opened_) {
        result.error = "回放文件尚未打开";
        return result;
    }

    const std::int64_t elapsed = SteadyNowMs() - start_ms_;

    // 时间没到的块一块都不吐，两次读取之间的间隔要照原样重现
    // 同一轮到期的块合成一次读取：串口一次 ReadFile 也是把当时缓冲里的字节全拿走
    while (next_chunk_ < chunks_.size() && elapsed >= chunks_[next_chunk_].at_ms) {
        result.bytes += chunks_[next_chunk_].bytes;
        last_feed_ms_ = SteadyNowMs();
        ++next_chunk_;
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
    return SteadyNowMs() - last_feed_ms_ >= kDrainMs;
}

} // namespace iotd
