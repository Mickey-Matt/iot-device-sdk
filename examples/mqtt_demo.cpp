// ===========================================================================
// examples/mqtt_demo.cpp
//
// 【示例程序 02】从模拟传感器读数，上报到 MQTT 云平台
//
// 【两条链路】
//   上行 ：MockSource → DeviceClient → MqttSink.Publish
//   下行 ：broker → MqttSink (OnMessageStatic,HandleMessage) → OnCommand
// 【注意】下行不经过DeviceClient —— 它只管"传感器数据往上走"这一条，
//         但是命令执行后的回执 cmd_ack 会经由 PublishRaw 上传至 broker
//
// 运行前先启动 broker：
//     mosquitto -p 1883 -v
//
// 然后运行本程序：
//     ./mqtt_demo                    # 连本机 broker
//     ./mqtt_demo 192.168.1.10       # 连指定 IP 的 broker
//
// 下行命令：
//     mosquitto_pub - h localhost - t dev/fish-tank-01/cmd -f samples/cmd_feed.json
//     mosquitto_pub - h localhost - t dev/fish-tank-01/cmd -f samples/cmd_light_on.json
// ===========================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "iotd/device_client.h"
#include "iotd/json.h"
#include "iotd/logger.h"
#include "iotd/mock_source.h"
#include "iotd/mqtt_sink.h"

#if defined(_WIN32)
#include <windows.h> // SetConsoleOutputCP
#endif

namespace {

std::atomic<bool> g_should_exit{false};

void OnSignal(int) {
    g_should_exit = true;
}

// ---------------------------------------------------------------------------
// 继承 MqttSink 来实现自己的 OnCommand 处理逻辑
// ---------------------------------------------------------------------------
class FishTankDevice : public iotd::MqttSink {
public:
    using iotd::MqttSink::MqttSink; // 继承基类的构造函数

    void OnCommand(const std::string& topic, const std::string& payload) override {
        // 先调基类默认实现（会打日志），再加上自己的业务逻辑
        iotd::MqttSink::OnCommand(topic, payload);

        std::string cmd = "unknown";
        try {
            const auto value = iotd::json::Parse(payload);
            cmd = iotd::json::GetString(value, "cmd", "unknown");
        } catch (const iotd::json::ParseError& e) {
            IOTD_LOG(iotd::Logger::Level::Warn,
                     "下发的命令不是合法 JSON（位置 " << e.position() << "）：" << e.what());
            return;
        }

        if (cmd == "feed") {
            LOG_INFO(">>> 执行喂食：投喂 2 克（这里本该驱动步进电机）");
        } else if (cmd == "light_on") {
            LOG_INFO(">>> 开启补光（这里本该拉高 PWM）");
        } else if (cmd == "light_off") {
            LOG_INFO(">>> 关闭补光");
        } else if (cmd == "pump_on") {
            LOG_INFO(">>> 开启水泵（这里本该闭合继电器）");
        } else {
            LOG_WARN(">>> 未知命令：" << cmd);
        }

        // 回执：让云端知道命令已收到并执行
        const std::string ack_topic = "dev/" + DeviceIdForAck() + "/cmd_ack";
        std::string error;
        if (PublishRaw(ack_topic, R"({"cmd":")" + cmd + R"(","result":"ok"})", error)) {
            LOG_INFO("已回执到 " << ack_topic);
        } else {
            LOG_WARN("回执失败：" << error);
        }
    }

    // 工具：把 device_id 暴露给示例用
    std::string DeviceIdForAck() const { return "fish-tank-01"; }
};

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    iotd::Logger::Instance().OpenFile("mqtt_demo.log", true);

    // --- 命令行参数：broker 地址 ---
    std::string host = "localhost";
    int port = 1883;
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = std::atoi(argv[2]);
        if (port <= 0) {
            port = 1883;
        }
    }

    LOG_INFO("=== mqtt_demo 启动（broker " << host << ":" << port << "）===");

    // --- 准备配置 ---
    iotd::MqttSink::Config config;
    config.broker_host = host;
    config.broker_port = port;
    config.device_id = "fish-tank-01";
    config.qos = 1;
    config.keepalive_sec = 15;          // 演示时缩短心跳，方便观察重连
    config.reconnect_delay_sec = 1;     // 首次重连等 1 秒
    config.reconnect_delay_max_sec = 8; // 最多等到 8 秒

    // 初始化 MqttSink 和 IDataSource
    FishTankDevice device(config);
    iotd::MockSource sensor(3000);
    // sensor.SetCorruptEveryN(-2); // 坏帧注入频率

    iotd::DeviceClient client(sensor, device);

    std::string error;
    if (!client.Start(error)) {
        LOG_ERROR("启动失败：" << error);
        LOG_ERROR("提示：确认 broker 已经启动 —— 在另一个窗口执行 'mosquitto -p 1883 -v'");
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    LOG_INFO("按 Ctrl+C 退出");
    LOG_INFO("测试下行链路（推荐用 -f 文件方式，命令行传 JSON 会被 shell 吃掉双引号）：");
    LOG_INFO("  mosquitto_pub -h localhost -t dev/fish-tank-01/cmd -f samples/cmd_feed.json");

    // --- 主循环 ---
    while (!g_should_exit.load()) {
        client.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.Stop();

    // --- 统计表 ---
    const auto& c = client.GetStats();
    const auto& s = device.GetStats();
    std::cout << "\n--- 上行（传感器 → 云）---\n"
              << "收到帧数   : " << c.frames_received << "\n"
              << "解析成功   : " << c.frames_ok << "\n"
              << "解析失败   : " << c.parse_failed << "  (坏帧被丢弃，程序继续跑)\n"
              << "上报失败   : " << c.publish_failed << "  (通常是断网期间)\n"
              << "\n--- 下行（云 → 设备）---\n"
              << "连接尝试   : " << s.connect_attempts << "\n"
              << "连接成功   : " << s.connect_success << "  (重连成功的次数也在这里)\n"
              << "收到下行   : " << s.messages_received << " 条\n"
              << "发布成功   : " << s.publish_ok << " 条\n"
              << "发布失败   : " << s.publish_failed << " 条\n";

    //   frames_received 涨但 frames_ok 不涨 → 数据源通了，但报文格式不对
    //   frames_received 根本不涨            → 链路没通，问题在设备侧
    //   连接成功>1                          → 发生过断线重连，说明网络不稳
    //   发布失败有值                        → 断网期间的数据没发出去（已丢弃）

    iotd::Logger::Instance().Close();
    return 0;
}
