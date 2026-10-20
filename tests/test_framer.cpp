// ===========================================================================
// tests/test_framer.cpp
//
// 分帧和录制的测试：半包、粘包、CRLF、超限各来一遍，再把五个现场样本回放一遍
// ===========================================================================

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "iotd/capture.h"
#include "iotd/capture_recorder.h"
#include "iotd/device_client.h"
#include "iotd/file_replay_source.h"
#include "iotd/line_framer.h"
#include "iotd/log_sink.h"
#include "test_check.h"

#if defined(_WIN32)
#include <windows.h> // SetConsoleOutputCP 需要它
#endif

namespace {

void AppendString(iotd::LineFramer& framer, const std::string& text) {
    framer.Append(text.data(), text.size());
}

// 取一帧，顺便断言"确实有一帧"
// 直接对空 optional 解引用是未定义行为，测试里宁可记一次失败也不要崩
std::string TakeFrame(iotd::LineFramer& framer) {
    const auto frame = framer.Next();
    CHECK(frame.has_value());
    return frame.value_or(std::string());
}

void TestHalfPacket() {
    std::printf("[测试] 半包：一帧分两次到，第一次不能交出任何东西\n");
    iotd::LineFramer framer;

    AppendString(framer, R"({"temp":25.)");
    CHECK(!framer.Next().has_value());
    CHECK_EQ(framer.BufferedBytes(), 11);

    AppendString(framer, "1}\n");
    const auto frame = framer.Next();
    CHECK(frame.has_value());
    CHECK_EQ_STR(*frame, R"({"temp":25.1})");
    // 一帧只交一次，交完缓冲区就该空了
    CHECK(!framer.Next().has_value());
    CHECK_EQ(framer.BufferedBytes(), 0);
}

void TestStickyPacket() {
    std::printf("[测试] 粘包：一次到三帧，切两帧留一截\n");
    iotd::LineFramer framer;

    AppendString(framer, "{\"a\":1}\n{\"b\":2}\n{\"c\":");

    const auto first = framer.Next();
    CHECK(first.has_value());
    CHECK_EQ_STR(*first, R"({"a":1})");

    const auto second = framer.Next();
    CHECK(second.has_value());
    CHECK_EQ_STR(*second, R"({"b":2})");

    CHECK(!framer.Next().has_value());
    CHECK_EQ(framer.BufferedBytes(), 5);
}

void TestCarriageReturn() {
    std::printf("[测试] CRLF：帧尾的 \\r 要去掉，但单独的 \\r 不算一帧\n");
    iotd::LineFramer framer;

    AppendString(framer, "abc\r\n");
    CHECK_EQ_STR(TakeFrame(framer), "abc");

    // 只有 \r 没有 \n：\r 不是帧边界，这一帧还没完
    AppendString(framer, "xyz\r");
    CHECK(!framer.Next().has_value());
    AppendString(framer, "\n");
    CHECK_EQ_STR(TakeFrame(framer), "xyz");
}

void TestEmptyLine() {
    std::printf("[测试] 空行也是一帧，连续两个换行就是两帧空帧\n");
    iotd::LineFramer framer;
    AppendString(framer, "\n\na\n");

    // 空行不能被悄悄跳过：跳过就等于替上层决定了"空行不算数据"
    CHECK_EQ_STR(TakeFrame(framer), "");
    CHECK_EQ_STR(TakeFrame(framer), "");
    CHECK_EQ_STR(TakeFrame(framer), "a");
    CHECK(!framer.Next().has_value());
}

void TestOversize() {
    std::printf("[测试] 超限：一直等不到换行就整块丢掉，丢掉后还能继续用\n");
    iotd::LineFramer framer(16);

    AppendString(framer, std::string(10, 'A'));
    CHECK(!framer.Next().has_value());
    CHECK_EQ(framer.BufferedBytes(), 10);
    CHECK_EQ(framer.OverflowBytes(), 0);

    AppendString(framer, std::string(10, 'B')); // 攒到 20 字节，超过 16
    CHECK(!framer.Next().has_value());
    CHECK_EQ(framer.BufferedBytes(), 0);
    CHECK_EQ(framer.OverflowBytes(), 20);

    AppendString(framer, "ok\n");
    CHECK_EQ_STR(TakeFrame(framer), "ok");
    CHECK_EQ(framer.OverflowBytes(), 20); // 丢过的字节数不会因为恢复而清零
}

void TestHalfFrameTimeout() {
    std::printf("[测试] 半帧超时：等到死也不来的后半截，到点就丢掉\n");
    // 超时设 5ms，测起来不用等太久
    iotd::LineFramer framer(8 * 1024, 5);

    AppendString(framer, R"({"temp":)");
    CHECK_EQ(framer.BufferedBytes(), 8);
    CHECK(!framer.Tick()); // 刚进来，还没超时
    CHECK_EQ(framer.TimeoutBytes(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(framer.Tick());
    CHECK_EQ(framer.BufferedBytes(), 0);
    CHECK_EQ(framer.TimeoutBytes(), 8);

    // 丢掉之后链路还能接着用
    AppendString(framer, "ok\n");
    CHECK_EQ_STR(TakeFrame(framer), "ok");
}

void TestTapKeepsChunkBoundary() {
    std::printf("[测试] 录制钩子：录到的是每次收到的字节块，不是切好的帧\n");
    std::vector<std::string> seen;
    iotd::LineFramer framer;
    framer.SetTap(
        [&seen](const char* data, std::size_t n) { seen.emplace_back(data, n); });

    AppendString(framer, "a\nb");
    AppendString(framer, "c\n");

    CHECK_EQ(seen.size(), 2);
    CHECK_EQ_STR(seen[0], "a\nb");
    // 如果钩子记的是整帧，这里会是 "c"；记字节块才能保留"这条帧是怎么断开的"
    CHECK_EQ_STR(seen[1], "c\n");
}

void TestCaptureEscapeRoundTrip() {
    std::printf("[测试] 录制格式：转义后能放进一行，解回来一个字节不差\n");

    std::string raw = "a\tb\r\\";
    raw.push_back('\0');
    raw.push_back('\x01');
    raw.push_back(static_cast<char>(0xFF));

    const std::string escaped = iotd::capture::Escape(raw.data(), raw.size());
    CHECK(escaped.find('\n') == std::string::npos);
    CHECK(escaped.find('\t') == std::string::npos);
    CHECK(escaped.find('\r') == std::string::npos);

    iotd::capture::Chunk chunk;
    std::string error;
    CHECK(iotd::capture::ParseLine("@1234\t" + escaped, chunk, error));
    CHECK_EQ(chunk.at_ms, 1234);
    CHECK(chunk.bytes == raw);
}

void TestCaptureParseRejects() {
    std::printf("[测试] 录制格式：坏行要报出错在哪，不能悄悄读进去\n");
    iotd::capture::Chunk chunk;
    std::string error;

    CHECK(!iotd::capture::ParseLine("1234\tabc", chunk, error));
    CHECK(!error.empty());

    CHECK(!iotd::capture::ParseLine("@12a\tb", chunk, error));
    CHECK(!iotd::capture::ParseLine("@12 abc", chunk, error));
    CHECK(!iotd::capture::ParseLine("@12\t\\q", chunk, error));
    CHECK(!iotd::capture::ParseLine("@12\t\\x2", chunk, error));
}

// 把一份录制文件按"一到就喂、喂完就取"的顺序过一遍分帧器，得到所有成形的帧
bool ReplayFile(const std::string& path, std::vector<std::string>& frames,
                std::size_t& dropped) {
    std::vector<iotd::capture::Chunk> chunks;
    std::string error;
    if (!iotd::capture::LoadFile(path, chunks, error)) {
        std::printf("  载入失败：%s\n", error.c_str());
        return false;
    }

    iotd::LineFramer framer;
    for (const auto& chunk : chunks) {
        framer.Append(chunk.bytes.data(), chunk.bytes.size());
        while (const auto frame = framer.Next()) {
            frames.push_back(*frame);
        }
    }
    dropped = framer.OverflowBytes();
    return true;
}

// 数一份样本里"能解析成报文"和"解析失败"的帧各有几条
void CheckSample(const char* path, std::size_t expect_ok, std::size_t expect_bad,
                 std::size_t expect_dropped) {
    std::printf("[测试] 样本 %s\n", path);

    std::vector<std::string> frames;
    std::size_t dropped = 0;
    if (!ReplayFile(std::string("samples/captures/") + path, frames, dropped)) {
        ++g_failed;
        return;
    }

    std::size_t ok = 0;
    std::size_t bad = 0;
    for (const auto& frame : frames) {
        iotd::DeviceMessage message;
        std::string error;
        if (iotd::DeviceClient::ParseFrame(frame, message, error)) {
            ++ok;
        } else {
            ++bad;
        }
    }

    CHECK_EQ(ok, expect_ok);
    CHECK_EQ(bad, expect_bad);
    CHECK_EQ(dropped, expect_dropped);
}

void TestSamples() {
    // 两帧粘在一起，两条都能解析
    CheckSample("sticky.txt", 2, 0, 0);
    // 一帧被拆成两次读，拼起来才好
    CheckSample("half.txt", 1, 0, 0);
    // 波特率配错：两条乱码帧，改对之后两条正常
    CheckSample("garbage.txt", 2, 2, 0);
    // 帧尾是 \r\n，两帧都能解析就说明 \r 被清掉了
    CheckSample("crlf.txt", 2, 0, 0);
    // 8500 字节等不到换行，整块丢弃，后面一帧照常解析
    CheckSample("oversize.txt", 1, 0, 8500);
}

void TestRecorderRoundTrip() {
    std::printf("[测试] 录制→回放：录的是字节块，回放出来的帧和现场一模一样\n");

    const std::string base = "record_tmp";
    iotd::CaptureRecorder recorder(base);
    std::string error;
    CHECK(recorder.Open(error));
    CHECK_EQ_STR(recorder.CurrentPath(), base + ".0");

    iotd::LineFramer framer;
    framer.SetTap(
        [&recorder](const char* data, std::size_t n) { recorder.OnBytes(data, n); });

    AppendString(framer, R"({"a":1})");
    AppendString(framer, "\n{\"b\":");
    AppendString(framer, "2}\n");
    recorder.Close();

    // 记的是原始字节数（7 + 6 + 3），不是"两帧"
    CHECK_EQ(recorder.RecordedBytes(), 16);
    CHECK_EQ(recorder.LostBytes(), 0);

    std::vector<std::string> frames;
    std::size_t dropped = 0;
    CHECK(ReplayFile(base + ".0", frames, dropped));
    CHECK_EQ(frames.size(), 2);
    if (frames.size() == 2) {
        CHECK_EQ_STR(frames[0], R"({"a":1})");
        CHECK_EQ_STR(frames[1], R"({"b":2})");
    }
    std::remove((base + ".0").c_str());
}

void TestRecorderRotation() {
    std::printf("[测试] 录制分段：写满一段就换下一段，写满一圈回头覆盖最旧的\n");

    iotd::CaptureRecorder recorder("rot_tmp", 8, 2); // 一段 8 字节，最多两段
    std::string error;
    CHECK(recorder.Open(error));

    recorder.OnBytes("12345678", 8);
    CHECK_EQ_STR(recorder.CurrentPath(), "rot_tmp.0");

    // 这一块下去会超出 8 字节，先换段再写
    recorder.OnBytes("9", 1);
    CHECK_EQ_STR(recorder.CurrentPath(), "rot_tmp.1");

    // 再换一次就绕回第 0 段，最旧的那段被丢掉
    recorder.OnBytes("01234567", 8);
    CHECK_EQ_STR(recorder.CurrentPath(), "rot_tmp.0");
    recorder.OnBytes("x", 1);
    CHECK_EQ_STR(recorder.CurrentPath(), "rot_tmp.1");
    recorder.Close();

    CHECK_EQ(recorder.RecordedBytes(), 18);
    std::remove("rot_tmp.0");
    std::remove("rot_tmp.1");
}

void TestRecorderOpenFailure() {
    std::printf("[测试] 录制打不开：不能影响数据链路，也不能崩\n");

    iotd::CaptureRecorder recorder("no_such_dir_here/field.cap");
    std::string error;
    CHECK(!recorder.Open(error));
    CHECK(!error.empty());
    CHECK(!recorder.IsActive());

    recorder.OnBytes("abc", 3);
    CHECK_EQ(recorder.RecordedBytes(), 0);
    recorder.Close();
}

// 测试用的假数据源：按脚本吐出字节块，不碰硬件也不碰文件
class ScriptedSource : public iotd::IDataSource {
public:
    bool Open(std::string&) override { return true; }
    void Close() override {}

    iotd::ByteChunk Poll() override {
        iotd::ByteChunk result;
        if (next_ < chunks_.size()) {
            result.bytes = chunks_[next_++];
            return result;
        }
        result.error = error_after_script_;
        return result;
    }

    std::string Name() const override { return "Scripted"; }

    void AddChunk(std::string bytes) { chunks_.push_back(std::move(bytes)); }
    void FailWithAfterScript(std::string message) {
        error_after_script_ = std::move(message);
    }

private:
    std::vector<std::string> chunks_;
    std::size_t next_ = 0;
    std::string error_after_script_; // 脚本吐完之后每轮都报这个错
};

void TestClientPipeline() {
    std::printf("[测试] 整条链路：字节块进来，帧出去，超限单独计数\n");

    const std::string good = R"({"device_id":"d1","temp":25.0,"humid":50.0})";
    ScriptedSource source;
    // 第一块：两帧粘在一起；第二块：一大团没有换行的垃圾
    source.AddChunk(good + "\n" + good + "\n");
    source.AddChunk(std::string(40, 'X'));

    iotd::LineFramer framer(16); // 上限调到 16 字节，第二块才能触发超限
    iotd::LogSink sink("test");
    iotd::DeviceClient client(source, framer, sink);

    std::string error;
    CHECK(client.Start(error));

    // 第一块里有两帧，一次 PollOnce 只取一帧，所以前两次都要有产出
    CHECK(client.PollOnce() == iotd::ProcessResult::Ok);
    CHECK(client.PollOnce() == iotd::ProcessResult::Ok);
    CHECK(client.PollOnce() == iotd::ProcessResult::NoData);

    const auto& stats = client.GetStats();
    CHECK_EQ(stats.frames_received, 2);
    CHECK_EQ(stats.frames_ok, 2);
    CHECK_EQ(stats.framer_overflow, 1);
    CHECK_EQ(stats.dropped_bytes, 40);
    CHECK_EQ(stats.transport_errors, 0);

    client.Stop();
}

void TestTransportErrorIsSeparate() {
    std::printf("[测试] 传输错误不记到分帧头上\n");

    ScriptedSource source;
    source.FailWithAfterScript("设备已拔出");
    iotd::LineFramer framer;
    iotd::LogSink sink("test");
    iotd::DeviceClient client(source, framer, sink);

    std::string error;
    CHECK(client.Start(error));
    CHECK(client.PollOnce() == iotd::ProcessResult::SourceError);

    const auto& stats = client.GetStats();
    CHECK_EQ(stats.transport_errors, 1);
    CHECK_EQ(stats.framer_overflow, 0);
    CHECK_EQ(stats.framer_timeout, 0);
    CHECK_EQ(stats.dropped_bytes, 0);

    client.Stop();
}

void TestFileReplaySource() {
    std::printf("[测试] FileReplaySource：只吐字节，帧由外面的分帧器切\n");

    const std::string path = "replay_tmp.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "# 测试用临时录制\n";
        out << "@0\t" << iotd::capture::Escape(R"({"temp":25.)", 11) << "\n";
        out << "@0\t" << iotd::capture::Escape("1}\n", 3) << "\n";
    }

    iotd::FileReplaySource source(path);
    std::string error;
    CHECK(source.Open(error));
    CHECK_EQ(source.ChunkCount(), 2);

    // 两块的时间戳都是 0，第一轮就会合成一次读取交出来
    const iotd::ByteChunk chunk = source.Poll();
    CHECK(chunk.error.empty());
    CHECK_EQ_STR(chunk.bytes, std::string(R"({"temp":25.)") + "1}\n");
    CHECK_EQ(source.FedChunks(), 2);

    iotd::LineFramer framer;
    framer.Append(chunk.bytes.data(), chunk.bytes.size());
    CHECK_EQ_STR(TakeFrame(framer), R"({"temp":25.1})");

    std::remove(path.c_str());
}

} // namespace

int main() {
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    std::printf("===== iotd 分帧与录制测试开始 =====\n");

    TestHalfPacket();
    TestStickyPacket();
    TestCarriageReturn();
    TestEmptyLine();
    TestOversize();
    TestHalfFrameTimeout();
    TestTapKeepsChunkBoundary();
    TestCaptureEscapeRoundTrip();
    TestCaptureParseRejects();
    TestSamples();
    TestRecorderRoundTrip();
    TestRecorderRotation();
    TestRecorderOpenFailure();
    TestClientPipeline();
    TestTransportErrorIsSeparate();
    TestFileReplaySource();

    std::printf("===== 测试结束：通过 %d，失败 %d =====\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
