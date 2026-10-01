// ===========================================================================
// tools/iot_debug_main.cpp
//
// 【调试命令行工具 iot-debug】
//
// 这是"技术支持工程师"日常真正会用的东西。
// 客户说"我的设备数据不对"，你不可能跑去现场，你需要一个能在自己电脑上
// 复现客户数据流的工具。这就是它的用途。
//
// 用法：
//   iot-debug list                列出本机可用串口
//   iot-debug ports               同上（别名）
//   iot-debug mock [间隔ms]       用模拟数据源跑，默认 500ms
//   iot-debug serial COM3 [波特率] 从真实串口读取并打印解析结果
//   iot-debug parse '{"a":1}'     只解析一段 JSON 并打印，用于快速验证
// ===========================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include "iotd/data_source.h"
#include "iotd/device_client.h"
#include "iotd/json.h"
#include "iotd/logger.h"
#include "iotd/mock_source.h"
#include "iotd/serial_port_source.h"

#if defined(_WIN32)
#include <windows.h>   // SetConsoleOutputCP 需要它
#endif

namespace {

std::atomic<bool> g_should_exit{false};

void OnSignal(int) { g_should_exit = true; }

void PrintUsage() {
    std::cout <<
        "iot-debug —— 物联网设备调试工具 v0.1\n"
        "\n"
        "用法:\n"
        "  iot-debug list                     列出本机可用串口\n"
        "  iot-debug mock [间隔ms]            用模拟数据源运行（默认 500ms）\n"
        "  iot-debug serial <端口> [波特率]   从串口读取并解析（默认 115200）\n"
        "  iot-debug parse <JSON文本>         解析一段 JSON 并打印结果\n"
        "  iot-debug parse --file <文件路径>  从文件读取 JSON 再解析（推荐）\n"
        "\n"
        "示例:\n"
        "  iot-debug mock 200\n"
        "  iot-debug serial COM3 115200\n"
        "  iot-debug parse --file sample.json\n"
        "\n"
        "【经验提示】直接命令行传 JSON 很容易被 shell 吃掉引号，\n"
        "  Windows PowerShell / cmd 都会。所以排查真实问题时优先用 --file。\n";
}

// 从文件读取全部内容，并去掉 UTF-8 BOM 和首尾空白。
//
// 为什么要去 BOM？
//   Windows 记事本、PowerShell 的 Out-File 都可能给文件加上 UTF-8 BOM
//   （开头三个字节 EF BB BF）。JSON 解析器看到这三个字节会直接报错，
//   而人在编辑器里完全看不出区别 —— 这是极难排查的一类问题。
//   把它处理掉，客户就少一个深夜找你的理由。
bool ReadTextFile(const std::string& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "无法打开文件：" + path;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    // 去掉 BOM
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }

    // 去掉首尾空白（换行、空格、制表符）
    const char* ws = " \t\r\n";
    const std::size_t begin = out.find_first_not_of(ws);
    if (begin == std::string::npos) {
        out.clear();
        return true;   // 空文件，交给解析器去报"期望一个值"
    }
    const std::size_t end = out.find_last_not_of(ws);
    out = out.substr(begin, end - begin + 1);
    return true;
}

// 统一的运行循环：让 mock 与 serial 两个子命令共用同一段驱动代码。
// 这是"消除重复"的工程习惯 —— 如果两个子命令各写一遍循环，
// 以后要改退出逻辑就得改两处，早晚会漏一处。
int RunLoop(iotd::IDataSource& source, const std::string& tag) {
    iotd::DeviceClient client(source, [&tag](const iotd::DeviceMessage& msg) {
        IOTD_LOG(iotd::Logger::Level::Info,
                 "[" << tag << "] " << msg.device_id
                     << " temp=" << msg.reading.temperature_c
                     << " humid=" << msg.reading.humidity_pct
                     << " light=" << msg.reading.light_lux);
    });

    std::string error;
    if (!client.Start(error)) {
        LOG_ERROR("启动失败：" << error);
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    LOG_INFO("按 Ctrl+C 退出");

    while (!g_should_exit.load()) {
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
    // 一眼就能看出是链路没通（收到帧数=0）还是数据格式错（解析失败数高）。
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
                  << "  device_id  = " << iotd::json::GetString(value, "device_id", "(缺失)") << "\n"
                  << "  temp       = " << iotd::json::GetNumber(value, "temp", -999) << "\n"
                  << "  humid      = " << iotd::json::GetNumber(value, "humid", -999) << "\n";
        return 0;
    } catch (const iotd::json::ParseError& e) {
        // 出错时打印"第几个字符出错"，而不是笼统地说"解析失败"。
        // 这个细节直接决定客户能不能自己定位问题。
        std::cerr << "解析失败：位置 " << e.position() << "：" << e.what() << "\n";
        std::cerr << "定位：\n  ";
        for (std::size_t i = 0; i < e.position(); ++i) std::cerr << ' ';
        std::cerr << "^\n";
        return 2;
    }
}

}  // namespace

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
        const std::string port = argv[2];
        const int baud = (argc >= 4) ? std::atoi(argv[3]) : 115200;
        iotd::SerialPortSource serial(port, baud > 0 ? baud : 115200);
        return RunLoop(serial, port);
    }

    if (cmd == "parse") {
        if (argc < 3) {
            std::cerr << "错误：parse 需要一段 JSON 文本，或用 --file 指定文件\n";
            return 1;
        }

        // 支持 --file：从文件读 JSON。
        // 为什么必须有这个模式？因为命令行传 JSON 会被 shell 吃掉引号，
        // 这是排查真实问题时最容易踩的坑（我们在文档里专门讲了这个案例）。
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
