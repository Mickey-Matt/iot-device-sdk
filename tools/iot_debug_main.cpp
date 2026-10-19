// ===========================================================================
// tools/iot_debug_main.cpp
//
// 【调试命令行工具 iot-debug】
//
// 这是"技术支持工程师"日常真正会用的东西
// 客户说"我的设备数据不对"，你不可能跑去现场，你需要一个能在自己电脑上
// 复现客户数据流的工具，这就是它的用途
//
// 用法：
//   iot-debug list                列出本机可用串口
//   iot-debug ports               同上（别名）
//   iot-debug mock [间隔ms]       用模拟数据源跑，默认 500ms
//   iot-debug serial COM3 [波特率] 从真实串口读取并打印解析结果
//   iot-debug serial COM3 --record 现场.cap   边读边把原始字节录下来
//   iot-debug replay 现场.cap     按录制时的时间间隔把字节块重新喂回去
//   iot-debug parse '{"a":1}'     只解析一段 JSON 并打印，用于快速验证
// ===========================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "iotd/capture_recorder.h"
#include "iotd/data_source.h"
#include "iotd/device_client.h"
#include "iotd/file_replay_source.h"
#include "iotd/json.h"
#include "iotd/log_sink.h"
#include "iotd/logger.h"
#include "iotd/mock_source.h"
#include "iotd/serial_port_source.h"

#if defined(_WIN32)
#include <windows.h> // SetConsoleOutputCP 需要它
#endif

namespace {

std::atomic<bool> g_should_exit{false};

void OnSignal(int) {
    g_should_exit = true;
}

void PrintUsage() {
    std::cout << "iot-debug —— 物联网设备调试工具 v0.1\n"
                 "\n"
                 "用法:\n"
                 "  iot-debug list                     列出本机可用串口\n"
                 "  iot-debug mock [间隔ms]            用模拟数据源运行（默认 500ms）\n"
                 "  iot-debug serial <端口> [波特率]   从串口读取并解析（默认 115200）\n"
                 "  iot-debug serial <端口> --record <文件>   边读边把原始字节录下来\n"
                 "  iot-debug replay <录制文件>        按录制时的时间间隔回放\n"
                 "  iot-debug parse <JSON文本>         解析一段 JSON 并打印结果\n"
                 "  iot-debug parse --file <文件路径>  从文件读取 JSON 再解析（推荐）\n"
                 "\n"
                 "示例:\n"
                 "  iot-debug mock 200\n"
                 "  iot-debug serial COM3 115200\n"
                 "  iot-debug serial COM3 --record field.cap\n"
                 "  iot-debug replay samples/captures/half.txt\n"
                 "  iot-debug parse --file sample.json\n"
                 "\n"
                 "【经验提示】直接命令行传 JSON 很容易被 shell 吃掉引号，\n"
                 "  Windows PowerShell / cmd 都会。所以排查真实问题时优先用 --file。\n";
}

// 从文件读取全部内容，并去掉 UTF-8 BOM 和首尾空白
//
// 为什么要去 BOM？
//   Windows 记事本、PowerShell 的 Out-File 都可能给文件加上 UTF-8 BOM
//   （开头三个字节 EF BB BF），JSON 解析器看到这三个字节会直接报错，
//   而人在编辑器里完全看不出区别 —— 这是极难排查的一类问题
//   把它处理掉，客户就少一个深夜找你的理由
bool ReadTextFile(const std::string& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "无法打开文件：" + path;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    // 去掉 BOM
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }

    // 去掉首尾空白（换行、空格、制表符）
    const char* ws = " \t\r\n";
    const std::size_t begin = out.find_first_not_of(ws);
    if (begin == std::string::npos) {
        out.clear();
        return true; // 空文件，交给解析器去报"期望一个值"
    }
    const std::size_t end = out.find_last_not_of(ws);
    out = out.substr(begin, end - begin + 1);
    return true;
}

// 统一的运行循环：mock / serial / replay 三个子命令共用同一段驱动代码
// 这是"消除重复"的工程习惯 —— 如果三个子命令各写一遍循环，
// 以后要改退出逻辑就得改三处，早晚会漏一处
//
// finished 为空表示一直跑到按 Ctrl+C；回放这类"有终点"的数据源传自己的结束判断
int RunLoop(iotd::IDataSource& source, const std::string& tag,
            const std::function<bool()>& finished = {}) {
    // 出口用 LogSink：调试工具只关心"数据长什么样"，不需要真的上云
    // tag 会作为日志前缀，这样几路输出能分得清
    iotd::LogSink sink(tag);
    iotd::DeviceClient client(source, sink);

    std::string error;
    if (!client.Start(error)) {
        LOG_ERROR("启动失败：" << error);
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    LOG_INFO("按 Ctrl+C 退出");

    while (!g_should_exit.load() && !(finished && finished())) {
        client.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.Stop();
    const auto& s = client.GetStats();
    std::cout << "\n--- 运行统计 ---\n"
              << "收到帧数 : " << s.frames_received << "\n"
              << "解析成功 : " << s.frames_ok << "\n"
              << "解析失败 : " << s.parse_failed << "\n"
              << "数据源错误: " << s.source_errors << "\n";
    // 这个统计表就是"技术支持"的交付物雏形：
    // 客户问"为什么我的数据丢了"，你把这张表拍给他，
    // 一眼就能看出是链路没通（收到帧数=0）还是数据格式错（解析失败数高）
    return 0;
}

int CmdReplay(const std::string& path) {
    iotd::FileReplaySource replay(path);
    const int rc = RunLoop(replay, "replay", [&replay] { return replay.Finished(); });
    if (rc != 0) {
        return rc;
    }

    std::cout << "\n--- 回放进度 ---\n"
              << "字节块   : " << replay.FedChunks() << " / " << replay.ChunkCount()
              << "\n"
              << "丢弃字节 : " << replay.DroppedBytes() << "\n"
              << "残留半帧 : " << replay.BufferedBytes() << " 字节\n";
    // 残留半帧不为 0 是正常的：录制正好停在"帧只到一半"的位置
    // 但如果它很大又是乱码，就要怀疑录制时的波特率跟设备对不上
    return 0;
}

int CmdList() {
    const auto ports = iotd::SerialPortSource::ListAvailablePorts();
    if (ports.empty()) {
        std::cout << "未发现可用串口。\n"
                     "提示：如果你没有硬件，用 'iot-debug mock' 即可跑通全流程。\n";
        return 0;
    }
    std::cout << "发现 " << ports.size() << " 个串口：\n";
    for (const auto& p : ports) {
        std::cout << "  " << p << "\n";
    }
    return 0;
}

int CmdParse(const std::string& text, const std::string& source_label) {
    std::cout << "输入来源：" << source_label << "\n";
    std::cout << "原文（" << text.size() << " 字节）：" << text << "\n\n";

    try {
        const auto value = iotd::json::Parse(text);
        std::cout << "解析成功：\n" << iotd::json::Dump(value) << "\n";

        // 顺便演示如何按字段取值
        std::cout << "\n字段提取：\n"
                  << "  device_id  = "
                  << iotd::json::GetString(value, "device_id", "(缺失)") << "\n"
                  << "  temp       = " << iotd::json::GetNumber(value, "temp", -999)
                  << "\n"
                  << "  humid      = " << iotd::json::GetNumber(value, "humid", -999)
                  << "\n";
        return 0;
    } catch (const iotd::json::ParseError& e) {
        // 出错时打印"第几个字符出错"，而不是笼统地说"解析失败"
        std::cerr << "解析失败：位置 " << e.position() << "：" << e.what() << "\n";

        // 定位连同错误原文一起打出来
        std::size_t begin = 0;
        if (e.position() > 0) {
            const auto prev_nl = text.rfind('\n', e.position() - 1);
            if (prev_nl != std::string::npos)
                begin = prev_nl + 1;
        }
        const auto next_nl = text.find('\n', e.position());
        const std::size_t end = (next_nl == std::string::npos) ? text.size() : next_nl;
        std::string line = text.substr(begin, end - begin);
        // 文件按 CRLF 存时行尾会多个 \r，会让 ^ 偏一格
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::cerr << "定位：\n  " << line << "\n  ";
        for (std::size_t i = begin; i < e.position(); ++i)
            std::cerr << ' ';
        std::cerr << "^\n";
        return 2;
    }
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    if (!iotd::Logger::Instance().OpenFile("iot-debug.log", true)) {
        std::cerr << "警告：无法写入 iot-debug.log\n";
    }

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string cmd = argv[1];

    if (cmd == "list" || cmd == "ports") {
        return CmdList();
    }

    if (cmd == "mock") {
        const int interval = (argc >= 3) ? std::atoi(argv[2]) : 500;
        iotd::MockSource mock(interval > 0 ? interval : 500);
        return RunLoop(mock, "mock");
    }

    if (cmd == "serial") {
        if (argc < 3) {
            std::cerr << "错误：serial 需要指定端口名，例如 'iot-debug serial COM3'\n";
            return 1;
        }

        // 端口和波特率是位置参数，--record 可以夹在中间，所以要自己走一遍
        std::string port;
        std::string record_path;
        int baud = 115200;
        bool baud_set = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--record") {
                if (i + 1 >= argc) {
                    std::cerr
                        << "错误：--record 后面要跟文件路径，例如 --record field.cap\n";
                    return 1;
                }
                record_path = argv[++i];
            } else if (port.empty()) {
                port = arg;
            } else if (!baud_set) {
                const int parsed = std::atoi(arg.c_str());
                baud = parsed > 0 ? parsed : baud;
                baud_set = true;
            }
        }

        iotd::SerialPortSource serial(port, baud);

        // 录制只挂在 serial 上：mock 产出的本来就是整帧，录下来也没有分帧证据
        iotd::CaptureRecorder recorder(record_path);
        if (!record_path.empty()) {
            std::string record_error;
            if (!recorder.Open(record_error)) {
                std::cerr << "警告：" << record_error << "，继续运行但不录制\n";
            } else {
                serial.SetTap([&recorder](const char* data, std::size_t n) {
                    recorder.OnBytes(data, n);
                });
                LOG_INFO("正在录制串口原始字节：" << recorder.CurrentPath());
            }
        }

        const int rc = RunLoop(serial, port);

        if (recorder.RecordedBytes() > 0) {
            LOG_INFO("录制结束：" << recorder.CurrentPath() << "，共 "
                                  << recorder.RecordedBytes() << " 字节");
            if (recorder.LostBytes() > 0) {
                LOG_WARN("录制期间有 " << recorder.LostBytes() << " 字节因写入失败丢失");
            }
        } else if (!record_path.empty()) {
            // 串口没打开或者一块字节都没收到，空文件留着只会让人以为录到了东西
            recorder.Close();
            const std::string empty_path = recorder.CurrentPath();
            std::remove(empty_path.c_str());
            LOG_WARN("录制没有任何内容，已删除空文件：" << empty_path);
        }
        return rc;
    }

    if (cmd == "replay") {
        if (argc < 3) {
            std::cerr << "错误：replay 需要指定录制文件，例如 "
                         "'iot-debug replay samples/captures/half.txt'\n";
            return 1;
        }
        return CmdReplay(argv[2]);
    }

    if (cmd == "parse") {
        if (argc < 3) {
            std::cerr << "错误：parse 需要一段 JSON 文本，或用 --file 指定文件\n";
            return 1;
        }

        // 支持 --file：从文件读 JSON
        // 为什么必须有这个模式？因为命令行传 JSON 会被 shell 吃掉引号，
        // 这是排查真实问题时最容易踩的坑（我们在文档里专门讲了这个案例）
        if (std::string(argv[2]) == "--file") {
            if (argc < 4) {
                std::cerr << "错误：--file 后面要跟文件路径\n";
                return 1;
            }
            std::string content;
            std::string read_error;
            if (!ReadTextFile(argv[3], content, read_error)) {
                std::cerr << read_error << "\n";
                return 3;
            }
            return CmdParse(content, std::string("文件 ") + argv[3]);
        }

        return CmdParse(argv[2], "命令行参数");
    }

    std::cerr << "未知命令：" << cmd << "\n\n";
    PrintUsage();
    return 1;
}
