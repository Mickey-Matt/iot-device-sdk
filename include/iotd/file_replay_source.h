// ===========================================================================
// iotd/file_replay_source.h
//
// 按录制文件里记下的时间间隔，把当时的字节块重新喂回分帧器
//
// 和串口共用 LineFramer，分帧路径完全相同
// 文件格式见 iotd/capture.h
// ===========================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iotd/capture.h"
#include "iotd/data_source.h"
#include "iotd/line_framer.h"

namespace iotd {

class FileReplaySource : public IDataSource {
public:
    explicit FileReplaySource(std::string path);

    bool Open(std::string& error) override;
    void Close() override;
    ReadResult Poll() override;
    std::string Name() const override;

    // 所有字节块都喂完了，而且缓冲区里不再有半帧，或最后一块之后已过了 kDrainMs
    // 后一种情况是为了让"文件最后是个半帧"的录制也能自然结束，不至于把回放卡死
    bool Finished() const;

    std::size_t ChunkCount() const { return chunks_.size(); }
    std::size_t FedChunks() const { return next_chunk_; }
    std::size_t BufferedBytes() const { return framer_.BufferedBytes(); }
    std::size_t DroppedBytes() const { return framer_.DroppedBytes(); }

private:
    std::string path_;
    std::vector<capture::Chunk> chunks_;
    std::size_t next_chunk_ = 0;
    LineFramer framer_;
    std::int64_t start_ms_ = 0;
    std::int64_t last_feed_ms_ = 0;
    bool opened_ = false;
};

} // namespace iotd
