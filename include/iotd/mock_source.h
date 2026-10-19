// ===========================================================================
// iotd/mock_source.h
// 模拟数据源
// ===========================================================================

#pragma once

#include <cstdint>
#include <random>

#include "iotd/data_source.h"

namespace iotd {

class MockSource : public IDataSource {
public:
    // interval_ms：每隔多少毫秒产出一帧
    explicit MockSource(int interval_ms = 1000);

    bool Open(std::string& error) override;
    void Close() override;
    ReadResult Poll() override;
    std::string Name() const override { return "MockSource"; }

    // 测试用的开关：让下一帧故意产出一段坏的 JSON
    void InjectCorruptedFrame() { inject_corrupted_ = true; }
    void SetCorruptEveryN(int);

private:
    std::string BuildFrame();

    int interval_ms_;
    int corrupt_every_n_ = 7;
    bool opened_ = false;
    bool inject_corrupted_ = false;
    std::uint64_t counter_ = 0;    // 第几帧
    std::int64_t next_due_ms_ = 0; // 下一帧该在什么时候产出
    std::mt19937 rng_;             // 随机数引擎
    std::uniform_real_distribution<double> temp_dist_;
    std::uniform_real_distribution<double> humid_dist_;
    std::uniform_real_distribution<double> light_dist_;
};

} // namespace iotd
