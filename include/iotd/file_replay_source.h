// ===========================================================================
// iotd/file_replay_source.h
//
// 按录制文件里记下的时间间隔，把当时的字节块重新吐出来
//
// 只搬字节，不分帧 —— 分帧由 DeviceClient 拿的那个 IFramer 做，和串口走同一条路径
// 文件格式见 iotd/capture.h
// ===========================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iotd/capture.h"
#include "iotd/data_source.h"

namespace iotd {

class FileReplaySource : public IDataSource {
public:
    explicit FileReplaySource(std::string path);

    bool Open(std::string& error) override;
    void Close() override;
    ByteChunk Poll() override;
    std::string Name() const override;

    // 所有字节块都喂完了，而且最后一块之后已经过了 kDrainMs
    // 这个余量是留给驱动循环的：一块里可能有好几帧，每调一次 PollOnce 只取走一帧
    bool Finished() const;

    std::size_t ChunkCount() const { return chunks_.size(); }
    std::size_t FedChunks() const { return next_chunk_; }

private:
    std::string path_;
    std::vector<capture::Chunk> chunks_;
    std::size_t next_chunk_ = 0;
    std::int64_t start_ms_ = 0;
    std::int64_t last_feed_ms_ = 0;
    bool opened_ = false;
};

} // namespace iotd
