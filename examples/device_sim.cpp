// ===========================================================================
// examples/device_sim.cpp
//
// 【示例程序 01】模拟一台设备持续上报数据
//
// 这是给"客户"看的第一份示例 —— 对应 JD 里的"完成 SDK 及示例程序的开发与交付"
// 好的示例程序有三个标准：
//   1. 短：30~80 行能看完，让人 5 分钟内跑起来
//   2. 完整：包含错误处理，不是"能跑就行"的 demo
//   3. 有注释：告诉使用者"为什么这里要这么写"
//
// 运行方式（在 build 目录下）：
//   ./device_sim            # 用内置模拟数据源，每 1 秒一帧
//   ./device_sim 500        # 每 500 毫秒一帧
//   ./device_sim 500 COM3   # 从 COM3 读真实串口，115200 波特率
// ===========================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "iotd/data_source.h"
#include "iotd/device_client.h"
#include "iotd/logger.h"
#include "iotd/mock_source.h"
#include "iotd/serial_port_source.h"

#if defined(_WIN32)
#include <windows.h> // SetConsoleOutputCP 需要它
#endif

namespace {

// 全局标志：收到 Ctrl+C 时置为 true，主循环据此优雅退出
//
// 为什么必须是 std::atomic？
//   因为信号可能在任何时刻、任何线程里打断程序
//   普通 bool 的读写不是原子的，可能读到"改了一半"的值
//   std::atomic<bool> 保证了读写的原子性，且不需要加锁
//   这是 C++ 并发编程里最基础也最重要的一个工具
std::atomic<bool> g_should_exit{false};

void OnSignal(int) {
    g_should_exit = true;
    // 注意：信号处理函数里只能做"异步信号安全"的操作
    // 也就是说，这里不能调用 printf、不能分配内存、不能加锁
    // 只设置一个标志位，是最稳妥的做法
}

// 上报回调：真正"把数据用起来"的地方
// 第一阶段：打印 + 落 CSV 文件；第二阶段这里会变成发 MQTT
void OnTelemetry(const iotd::DeviceMessage& msg) {
    IOTD_LOG(iotd::Logger::Level::Info,
             "上报 device=" << msg.device_id << " temp=" << msg.reading.temperature_c << "C"
                            << " humid=" << msg.reading.humidity_pct << "%"
                            << " light=" << msg.reading.light_lux << "lux"
                            << " ts=" << msg.timestamp_ms);
}

} // namespace

int main(int argc, char** argv) {
    // 让控制台支持 UTF-8 输出，否则中文日志会变乱码
    // Windows 控制台默认是 GBK 代码页，这是中文开发者最常踩的坑之一
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    // --- 解析命令行参数（不引第三方库，手写最简单的一版）---
    int interval_ms = 1000;
    std::string port_name;

    if (argc >= 2) {
        interval_ms = std::atoi(argv[1]);
        if (interval_ms <= 0) {
            std::cerr << "参数错误：间隔必须是正整数（毫秒）\n";
            return 1;
        }
    }
    if (argc >= 3) {
        port_name = argv[2];
    }

    // --- 打开日志文件 ---
    // 文件名带时间会让每次运行产生新文件；这里用固定名 + 追加模式，
    // 便于连续观察多次运行的结果
    if (!iotd::Logger::Instance().OpenFile("device_sim.log", /*append=*/true)) {
        std::cerr << "警告：无法打开日志文件，只输出到屏幕\n";
    }

    LOG_INFO("=== device_sim 启动 ===");

    // --- 选择数据源 ---
    // 这里体现了 IDataSource 接口的价值：
    // 下面的 DeviceClient 完全不知道数据是从模拟器还是串口来的
    iotd::IDataSource* source = nullptr;
    iotd::MockSource mock_source(interval_ms);
    iotd::SerialPortSource serial_source(port_name, 115200);

    if (port_name.empty()) {
        source = &mock_source;
        LOG_INFO("使用模拟数据源（没有指定串口）");
    } else {
        source = &serial_source;
        LOG_INFO("使用串口数据源：" << port_name);
    }

    iotd::DeviceClient client(*source, OnTelemetry);
    // 注意这里传的是 *source（解引用成引用）
    // DeviceClient 只是"借用"这个对象，不负责销毁它 ——
    // mock_source / serial_source 是栈上的局部变量，它们会自己析构

    std::string error;
    if (!client.Start(error)) {
        LOG_ERROR("启动失败：" << error);
        return 1;
    }

    std::signal(SIGINT, OnSignal);

    LOG_INFO("开始采集，按 Ctrl+C 优雅退出……");

    // --- 主循环 ---
    // 为什么用 while + sleep 而不是死循环？
    //   1. 给 CPU 留出时间（死循环会占满一个核心）
    //   2. 有时间片去检查 g_should_exit，实现优雅退出
    //   真实项目里通常会把这里换成事件循环或定时器
    while (!g_should_exit.load()) {
        client.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.Stop();

    const auto& stats = client.GetStats();
    LOG_INFO("=== 运行结束 ===");
    LOG_INFO("总计：收到 " << stats.frames_received << " 帧，解析成功 " << stats.frames_ok
                           << " 帧，失败 " << stats.parse_failed << " 帧，数据源错误 "
                           << stats.source_errors << " 次");

    // 打印"成功率"这个指标，是运维思维的体现：
    // 光说"跑完了"没有价值，说"成功率 87.5%"才能让人判断链路健不健康
    if (stats.frames_received > 0) {
        const double ok_rate = 100.0 * static_cast<double>(stats.frames_ok) /
                               static_cast<double>(stats.frames_received);
        LOG_INFO("解析成功率：" << ok_rate << "%");
    }

    iotd::Logger::Instance().Close();
    return 0;
}
