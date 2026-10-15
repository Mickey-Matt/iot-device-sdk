// ===========================================================================
// examples/mqtt_demo.cpp
//
// 【示例程序 02】从模拟传感器读数，上报到 MQTT 云平台
//
// 这是一个"完整网关"的最小形态：
//
//     MockSource ──▶ DeviceClient ──▶ MqttSink ──▶ broker
//     （数据来源）   解析/校验/贴标签     （上报出口）
//
// 主循环里只调了 client.PollOnce() 一行，上面那条流水线全在它里面。
// 想换成真硬件，把 MockSource 换成 SerialPortSource 即可，其余一行不用改
//
// 【两条链路走的是不同的路】
//   上行（传感器 → 云）  ：MockSource → DeviceClient → MqttSink.Publish
//   下行（云 → 你的代码）：broker → MqttSink 内部 → OnCommand 虚函数
// 下行【不经过】DeviceClient —— 它只管"传感器数据往上走"这一条
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
// 继承 MqttSink 来实现自己的命令处理逻辑
//
// 这就是头文件里把 OnCommand 设计成 virtual 的用途：
// 基类负责"把消息收进来"，子类负责"收到之后干什么"
// 分工清晰，而且不用改基类一行代码
// ---------------------------------------------------------------------------
class FishTankDevice : public iotd::MqttSink {
public:
    using iotd::MqttSink::MqttSink; // 继承基类的构造函数

    void OnCommand(const std::string& topic, const std::string& payload) override {
        // 先调基类默认实现（会打日志），再加上自己的业务逻辑
        iotd::MqttSink::OnCommand(topic, payload);

        // 真实设备在这里会：解析命令 JSON → 操作 GPIO/继电器 → 回一条执行结果
        // 我们这里只做"命令分发"的演示，并把执
        // 行结果回执发回云端（这也是真实项目的标准做法：
        // 云端必须知道命令到底有没有被执行，不能发出去就不管了）
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
        // 用的是【传输层】的 PublishRaw，不是业务层的 Publish ——
        // 回执不是遥测消息，直接发字节就够了
        const std::string ack_topic = "dev/" + DeviceIdForAck() + "/cmd_ack";
        std::string error;
        if (PublishRaw(ack_topic, R"({"cmd":")" + cmd + R"(","result":"ok"})", error)) {
            LOG_INFO("已回执到 " << ack_topic);
        } else {
            LOG_WARN("回执失败：" << error);
        }
    }

    // 小工具：把 device_id 暴露给示例用（真实 SDK 里可以直接放 getter）
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
    config.qos = 1;                     // 至少一次，不丢
    config.keepalive_sec = 15;          // 演示时缩短心跳，方便观察重连
    config.reconnect_delay_sec = 1;     // 首次重连等 1 秒（单位是秒！）
    config.reconnect_delay_max_sec = 8; // 最多等到 8 秒（演示时不用等太久）

    // --- 准备两个对象：一个"从哪读"，一个"往哪发" ---
    FishTankDevice device(config); // 既当数据源，也当上报出口（同一个 socket 两用）
    iotd::MockSource sensor(3000); // 每 3 秒造一帧假数据，将来换成 SerialPortSource

    // --- 把它们交给 DeviceClient ---
    // 注意两个参数的类型不一样：
    //   第一个是 IDataSource&      —— 它从这读
    //   第二个是 ITelemetrySink&   —— 它往这发
    // 而 device 这一个对象同时满足两个接口，所以两处都传它
    // （标准库的 std::iostream 就是这个套路：一个对象，既能读又能写）
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

    // --- 主循环：整条上行链路就靠这一行驱动 ---
    // client.PollOnce() 内部依次做五件事：
    //   ① source_.Poll()   去 MockSource 要一帧（3 秒才产一帧，其余时候返回"没数据"）
    //   ② ParseFrame()     把 JSON 文本解成 DeviceMessage
    //   ③ 范围校验          温度 -40~80、湿度 0~100，超了就丢弃并打警告
    //   ④ 贴标签            补上 uptime_ms 和 sequence
    //   ⑤ sink_.Publish()  交给出口，由 MqttSink 拼 JSON 发到 broker
    //
    // 【注意】网络收发【不在】这个循环里
    // 用 mosquitto_loop_start() 之后，收发消息、心跳、自动重连全部由
    // 库自己的后台线程负责
    //
    // 那 20ms 这个间隔还有什么意义？它只决定"我们多快响应一条下发命令"，
    // 20ms ≈ 几乎无延迟；就算改成 500ms，也只是命令晚半秒被执行，
    // 不影响连接可靠性，也不影响上报节奏（上报节奏由 MockSource 的间隔决定）
    while (!g_should_exit.load()) {
        client.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.Stop();

    // --- 收尾统计：两个方向各看一张表 ---
    // 上行这张在 client 里（有多少帧没解析出来、多少没发出去）
    const auto& c = client.GetStats();
    // 下行这张在 device 里（连接了几次、收了几条命令）
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
    // 这张表怎么用：
    //   frames_received 涨但 frames_ok 不涨 → 数据源通了，但报文格式不对
    //   frames_received 根本不涨            → 链路没通，问题在设备侧
    //   连接成功>1                          → 发生过断线重连，说明网络不稳
    //   发布失败有值                        → 断网期间的数据没发出去（已丢弃）

    iotd::Logger::Instance().Close();
    return 0;
}
