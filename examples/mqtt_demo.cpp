// ===========================================================================
// examples/mqtt_demo.cpp
//
// 【示例程序 02】通过 MQTT 上报数据并接收云端命令。
//
// 这是第二期的"验收程序"：跑起来能看到完整双向链路。
//
// 运行前先启动 broker（另开一个 MSYS2 UCRT64 窗口）：
//     mosquitto -p 1883 -v
//
// 然后运行本程序：
//     ./mqtt_demo                    # 连本机 broker
//     ./mqtt_demo 192.168.1.10       # 连指定 IP 的 broker
//
// 再从另一个窗口发一条下行命令，看它能不能收到：
//     mosquitto_pub -h localhost -t dev/fish-tank-01/cmd -m "{\"cmd\":\"feed\",\"amount\":2}"
// ===========================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "iotd/device_client.h"
#include "iotd/json.h"
#include "iotd/logger.h"
#include "iotd/mqtt_source.h"

#if defined(_WIN32)
#include <windows.h> // SetConsoleOutputCP
#endif

namespace {

std::atomic<bool> g_should_exit{false};

void OnSignal(int) {
    g_should_exit = true;
}

// ---------------------------------------------------------------------------
// 继承 MqttSource 来实现自己的命令处理逻辑。
//
// 这就是头文件里把 OnCommand 设计成 virtual 的用途：
// 基类负责"把消息收进来"，子类负责"收到之后干什么"。
// 分工清晰，而且不用改基类一行代码。
// ---------------------------------------------------------------------------
class FishTankDevice : public iotd::MqttSource {
public:
    using iotd::MqttSource::MqttSource; // 继承基类的构造函数

    void OnCommand(const std::string& topic, const std::string& payload) override {
        // 先调基类默认实现（会打日志），再加上自己的业务逻辑
        iotd::MqttSource::OnCommand(topic, payload);

        // 真实设备在这里会：解析命令 JSON → 操作 GPIO/继电器 → 回一条执行结果。
        // 我们这里只做"命令分发"的演示，并把执
        // 行结果回执发回云端（这也是真实项目的标准做法：
        // 云端必须知道命令到底有没有被执行，不能发出去就不管了）。
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
        if (Publish(ack_topic, R"({"cmd":")" + cmd + R"(","result":"ok"})", error)) {
            LOG_INFO("已回执到 " << ack_topic);
        } else {
            LOG_WARN("回执失败：" << error);
        }
    }

    // 小工具：把 device_id 暴露给示例用（真实 SDK 里可以直接放 getter）
    std::string DeviceIdForAck() const { return "fish-tank-01"; }
};

// 上报回调：MQTT 场景下，这里做的事情就是"把数据发出去"
void OnTelemetry(iotd::MqttSource& source, const iotd::DeviceMessage& msg) {
    std::string error;
    if (source.PublishTelemetry(msg.reading, error)) {
        IOTD_LOG(iotd::Logger::Level::Info,
                 "↑ 上报成功 temp=" << msg.reading.temperature_c
                                    << "C humid=" << msg.reading.humidity_pct << "%");
    } else {
        // 这条日志在断网时会出现 —— 它就是"链路出问题"的第一手证据
        IOTD_LOG(iotd::Logger::Level::Warn, "↑ 上报失败：" << error);
    }
}

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
    iotd::MqttSource::Config config;
    config.broker_host = host;
    config.broker_port = port;
    config.device_id = "fish-tank-01";
    config.qos = 1;                     // 至少一次，不丢
    config.keepalive_sec = 15;          // 演示时缩短心跳，方便观察重连
    config.reconnect_delay_sec = 1;     // 首次重连等 1 秒（单位是秒！）
    config.reconnect_delay_max_sec = 8; // 最多等到 8 秒（演示时不用等太久）

    FishTankDevice device(config);

    // --- 我们的数据源就是 MQTT，所以直接把它交给 DeviceClient ---
    // 注意：DeviceClient 完全不知道这是 MQTT，它只认 IDataSource 接口。
    // 这就是第一期做抽象的回报 —— 换数据源不用改一行核心代码。
    iotd::DeviceClient client(
        device, [&device](const iotd::DeviceMessage& msg) { OnTelemetry(device, msg); });

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
    // 两个职责：
    //   1. client.PollOnce() → 内部调 MqttSource::Poll() → 从待投递队列取一条命令
    //   2. 每 2 秒主动上报一次遥测
    //
    // 【注意】网络收发【不在】这个循环里了。
    // 用 mosquitto_loop_start() 之后，收发消息、心跳、自动重连全部由
    // 库自己的后台线程负责，主循环只做"取数据 + 上报"两件事。
    // （这个文件最早写的是"20ms 间隔不能放大，否则 PUBACK 处理不过来"，
    //   那是起手时用手动 mosquitto_loop() 的结论；改用 loop_start 之后
    //   已经不再成立，留着只会误导人，所以删掉。）
    //
    // 那 20ms 这个间隔现在还有意义吗？有，但含义变了：
    //   它只决定"我们多快响应一条下发命令"。20ms ≈ 几乎无延迟。
    //   就算改成 500ms，也只是命令晚半秒被执行，不影响连接可靠性。
    std::atomic<int> tick{0};
    while (!g_should_exit.load()) {
        client.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // 每 100 轮（约 2 秒）产生并上报一条模拟数据
        if (++tick % 100 == 0) {
            const int n = tick.load() / 100;
            iotd::SensorReading reading;
            reading.temperature_c = 24.0 + (n % 5) * 0.5;
            reading.humidity_pct = 60.0 + (n % 7);
            reading.light_lux = 300 + (n % 50);
            client.PublishTelemetry(reading);
        }
    }

    client.Stop();

    // --- 收尾统计：这就是面试时要展示的那张表 ---
    const auto& s = device.GetStats();
    std::cout << "\n--- MQTT 运行统计 ---\n"
              << "连接尝试   : " << s.connect_attempts << "\n"
              << "连接成功   : " << s.connect_success << "  (重连成功的次数也在这里)\n"
              << "收到下行   : " << s.messages_received << " 条\n"
              << "发布成功   : " << s.publish_ok << " 条\n"
              << "发布失败   : " << s.publish_failed << " 条\n";
    // 这张表怎么用：
    //   连接成功=1 且不再增长，但发布成功也不涨 → 根本没连上
    //   连接成功>1                          → 发生过断线重连，说明网络不稳
    //   发布失败有值                        → 断网期间的数据没发出去（已丢弃）

    iotd::Logger::Instance().Close();
    return 0;
}
