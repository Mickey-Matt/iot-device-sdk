// ===========================================================================
// iotd/capture_recorder.h
//
// 把串口收到的原始字节块按时间戳写进录制文件，格式和 FileReplaySource 读的完全一致
//
// 用法：现场让客户开着录制跑一遍，把文件发回来，这边用 iot-debug replay 就能重现
// ===========================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

namespace iotd {

class CaptureRecorder {
public:
    // 文件名会写成 path.0、path.1 ……
    // max_segment_bytes：单段写满就换下一段
    // max_segments：最多留几段，再换就覆盖最旧的一段（环形）
    // 【注意】换段是按字节数切的，不看帧边界，一帧可能被劈在两段里；
    //        单段单独回放时开头那半帧会被当成坏帧，这是固定长度切段的固有代价
    CaptureRecorder(std::string path, std::size_t max_segment_bytes = 4 * 1024 * 1024,
                    int max_segments = 4);
    ~CaptureRecorder();

    CaptureRecorder(const CaptureRecorder&) = delete;
    CaptureRecorder& operator=(const CaptureRecorder&) = delete;

    // 打开第一段。失败时返回 false，调用方继续跑，只是没有录制
    bool Open(std::string& error);
    void Close();

    // 直接挂到 LineFramer::SetTap
    void OnBytes(const char* data, std::size_t n);

    bool IsActive() const { return active_; }
    std::size_t RecordedBytes() const { return recorded_bytes_; }
    std::size_t LostBytes() const { return lost_bytes_; } // 写失败之后再来的字节
    const std::string& CurrentPath() const { return current_path_; }

private:
    bool OpenSegment(int index, std::string& error);
    bool Rotate();

    std::string base_path_;
    std::size_t max_segment_bytes_;
    int max_segments_;
    int segment_ = 0;
    std::ofstream out_;
    std::int64_t start_ms_ = 0; // 本段的时间零点，换段时重置
    std::size_t segment_bytes_ = 0;
    std::size_t recorded_bytes_ = 0;
    std::size_t lost_bytes_ = 0;
    bool active_ = false;
    std::string current_path_;
};

} // namespace iotd
