// ===========================================================================
// iotd/mqtt_source.h
//
// 【第二期核心文件】把 MQTT 接入进来。
//
// 注意这个文件里【没有】动 device_client.h 一行 —— 这正是第一期做
// "数据源抽象"的回报。MqttSource 只是 IDataSource 的第三个实现：
//
//                     IDataSource
//            ┌──────────────┼──────────────┐
//      MockSource   SerialPortSource   MqttSource   ← 第二期新增，仅此而已
//
// 数据流：
//   云端下发命令 ──订阅 dev/{id}/cmd──▶ 回调 → 队列 → Poll() → DeviceClient
//                                      （mosquitto 网络线程/回调）
//   设备数据上报 ◀──发布 dev/{id}/up─── PublishTelemetry()
//
// 【为什么必须用 mosquitto_loop_start（后台线程）而不是 mosquitto_loop（单线程）？】
//
// 这是我实际踩过的坑，写下来给你省几个小时：
//
// 我最初用 mosquitto_loop() 在主循环里手动驱动网络，理由听起来很合理——
// "回调都在同一个线程里执行，完全不用加锁，对新手更友好"。
// 结果实测断线重连时发现：
//   1. broker 挂掉后，mosquitto_loop() 每次返回 MOSQ_ERR_NO_CONN，
//      主循环 20ms 一轮 → 错误日志每秒刷 50 条，把日志文件淹掉
//   2. 更致命的是【它永远不会重连】——broker 重启后客户端依然躺着不动
//
// 查官方头文件（mosquitto.h 第 1207-1216 行的 Network loop 章节）才明白：
//   "The two recommended approaches are to use either mosquitto_loop_forever
//    or mosquitto_loop_start."
//   "It [mosquitto_loop_forever] handles reconnecting in case server connection
//    is lost."
// 原来【自动重连是 loop_forever / loop_start 提供的能力】，手动调 loop() 没有。
// 而且文档明确写了：当你还有别的任务要跑（比如读传感器）时，就该用 loop_start。
//
// 所以正确做法是：
//   mosquitto_loop_start()  → 库自己起一个后台线程跑网络循环 + 自动重连
//   我们的主线程           → 只管从队列取数据、发布数据
//
// 代价：回调（收消息、连接状态变化）现在发生在【后台线程】，
//       而 Poll()/Publish() 在【主线程】调用 —— 两者会同时访问
//       队列、连接标志、统计计数，所以【必须加锁】。
//       这个代价是必须付的，不能为了"避免多线程"而牺牲自动重连。
// ===========================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 需要 mosquitto.h 才能声明 struct mosquitto * 成员。
// 由 CMake 负责把 MSYS2 的 include 目录加进来（见 CMakeLists.txt）。
#include <mosquitto.h>

#include "iotd/data_source.h"
#include "iotd/types.h"

namespace iotd {

// ---------------------------------------------------------------------------
// MqttConfig：MQTT 连接配置
//
// 【为什么定义在类外面，而不是写成 MqttSource::Config 嵌套类？】
// 因为踩过一个真实的编译错误：如果 Config 是嵌套类，
// 那么 MqttSource 自己的构造函数就不能写 `MqttSource(Config config = Config{})` ——
// 那要求在 Config 所在的外层类【还没定义完】之前就使用 Config 的默认成员初始化器，
// GCC 16 会直接报错：
//   "default member initializer for 'broker_host' required before the end of
//    its enclosing class"
// 把 Config 提到命名空间层级，它就是一个完整的类型，怎么用都没问题。
// 类里再用 using Config = MqttConfig; 保留 MqttSource::Config 这种写法。
// ---------------------------------------------------------------------------
struct MqttConfig {
    std::string broker_host = "localhost"; // broker 地址，本机就是 localhost
    int broker_port = 1883;                // 1883 = MQTT 明文端口；8883 = TLS
    std::string client_id;                 // 留空则自动生成唯一 ID
    std::string username;                  // 需要鉴权时填写
    std::string password;
    std::string device_id = "fish-tank-01"; // 用于拼接主题
    int keepalive_sec = 60;                 // 心跳间隔：多久没交互就发 PINGREQ

    // --- QoS 选型（面试必答）---
    // 0 = 至多一次：可能丢，不重发
    // 1 = 至少一次：可能重复，但不会丢   ← 我们选它
    // 2 = 恰好一次：握手两次，开销最大
    // 设备遥测能容忍"偶发重复"（后端去重即可），但不能容忍"丢"，
    // 而 QoS 2 的开销对嵌入式设备不划算。所以 QoS 1 是工程上的平衡点。
    int qos = 1;

    // --- 重连退避 ---
    // 断线后先等 1 秒重试；还失败就翻倍（2s、4s…），最多等到 30 秒。
    // 为什么必须设上限？想象 broker 挂了 1 小时：
    //   没有退避 → 客户端每秒发起几千次连接，把自己的 CPU 和网络打满
    //   有退避   → 连接尝试频率被压到每分钟 2 次，几乎不耗资源
    // 这就是"重连风暴"的防治手段，也是面试里很能讲的一点。
    //
    // 【单位是秒！这是踩过的坑】
    // mosquitto_reconnect_delay_set 的文档参数说明写的是
    // "reconnect_delay - the number of seconds to wait between reconnects"，
    // 我第一版想当然按毫秒传了 1000，结果变成"等 1000 秒再重连"（约 17 分钟），
    // 实测断线后 42 秒都没恢复。改成 1 之后重连立刻正常。
    // 教训：凡是"间隔/时长"参数，先确认单位再填数。
    int reconnect_delay_sec = 1;
    int reconnect_delay_max_sec = 30;

    // --- 下行消息要不要也当作"数据帧"交给 DeviceClient？---
    //
    // false（默认，推荐）：命令只触发 OnCommand 回调，立即处理。
    //   ✓ 语义正确：命令是"事件"，不是"数据流"
    //   ✓ 不会被 DeviceClient 当成遥测帧去解析，避免无意义的解析失败
    //   ✓ 这就是"命令式消息"和"数据式消息"的区别，是 MQTT 设计里的核心概念
    //
    // true：命令也会进待投递队列。
    //   仅在你确实需要"把云端下发的内容也当作一路数据源"时才打开
    //   （比如云端推送的是配置块，你要存下来而不是立刻执行）。
    bool queue_downlink = false;
};

class MqttSource : public IDataSource {
public:
    using Config = MqttConfig; // 保留 MqttSource::Config 这种写法

    // -----------------------------------------------------------------------
    // 运行统计：排查现场问题的第一手依据
    // -----------------------------------------------------------------------
    struct Stats {
        std::uint64_t connect_attempts = 0;  // 发起连接次数
        std::uint64_t connect_success = 0;   // 连接成功次数（含自动重连成功）
        std::uint64_t messages_received = 0; // 收到的下行消息数
        std::uint64_t publish_ok = 0;        // 发布成功次数
        std::uint64_t publish_failed = 0;    // 发布失败次数
    };

    explicit MqttSource(Config config = Config{});
    ~MqttSource() override;

    // --- 实现 IDataSource 接口 ---
    bool Open(std::string& error) override;
    void Close() override;
    ReadResult Poll() override;
    std::string Name() const override;

    // --- MQTT 特有：上行发布 ---
    // 把一条遥测数据发布到 dev/{device_id}/up。
    // 返回 false 时 error 里会写明原因（未连接 / 报文过大 / 队列满）。
    bool PublishTelemetry(const SensorReading& reading, std::string& error);

    // 发布任意字符串到指定主题（调试用，比如手工注入一条命令）
    bool Publish(const std::string& topic, const std::string& payload, std::string& error);

    // 供 device_sim / iot-debug 查询连接状态
    // 【注意】这个标志会被后台线程改写，所以用 std::atomic 而不是 bool。
    bool IsConnected() const { return connected_.load(); }

    // 返回统计快照。为什么要"快照"而不是返回引用？
    // 因为统计计数会被后台线程修改，直接返回引用会让调用方读到"改了一半"的数据。
    Stats GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    // -----------------------------------------------------------------------
    // 下行命令回调
    //
    // 为什么在这里用 virtual 而不是 std::function？
    //   DeviceClient 用 std::function（因为它是"一个数据出口"，很轻）
    //   MqttSource 这里用 virtual（因为需要继承扩展的场景更重，且
    //   虚函数在 SDK 里更常见、更容易被 C++ 使用者理解）
    //   两种都是合法设计，能说清"为什么这里选这个"才是重点。
    //
    // 默认实现只打日志。要处理命令就继承并 override。
    // -----------------------------------------------------------------------
    virtual void OnCommand(const std::string& topic, const std::string& payload);

protected:
    // 给子类用的工具：往命令主题发一条下行指令
    std::string CommandTopic() const;
    std::string UpTopic() const;

private:
    // 回调必须是静态函数或自由函数（C 库不认 C++ 成员函数指针），
    // 所以用 static + void* 把 this 传进来 —— 这是 C 回调的经典写法。
    static void OnConnectStatic(struct mosquitto* mosq, void* obj, int rc);
    static void OnDisconnectStatic(struct mosquitto* mosq, void* obj, int rc);
    static void OnMessageStatic(struct mosquitto* mosq, void* obj,
                                const struct mosquitto_message* msg);

    // 真正的处理逻辑（非静态，能访问成员）
    void HandleConnect(int rc);
    void HandleDisconnect(int rc);
    void HandleMessage(const std::string& topic, const std::string& payload);

    Config config_;
    struct mosquitto* mosq_ = nullptr;

    // -----------------------------------------------------------------------
    // 多线程保护
    //
    // 用 mosquitto_loop_start() 后，收发消息和连接状态变化的回调都跑在
    // 库自己的后台线程里，而 Poll()/Publish() 在主线程 —— 两边会同时碰
    // 下面这些数据，所以必须保护。
    //
    // 为什么 connected_ 用 atomic 而 stats_ 用 mutex？
    //   connected_ 只是一个 bool 标志，atomic 足够且不会阻塞；
    //   stats_ 是一个结构体（5 个计数器），要保证"读到的是一致的一组值"，
    //   必须用锁。这是"按数据特点选同步手段"的判断，不是随便选的。
    // -----------------------------------------------------------------------
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_; // 保护 stats_ 与 pending_frames_
    Stats stats_;

    // 待投递队列：后台线程收到消息后放这里，主线程 Poll() 取走。
    // 【推模型转拉模型】MQTT 是回调驱动（消息来了就推给你），
    // 而 IDataSource 的契约是"我说要你才给我"（拉模型）。
    // 队列就是这个转换器。
    std::vector<std::string> pending_frames_;

    // 供 Open() 等待首次连接结果用：
    // 后台线程连上（或失败）后唤醒主线程，避免主线程死等。
    std::mutex connect_mutex_;
    std::condition_variable connect_cv_;
    bool connect_attempt_done_ = false;
    bool connect_succeeded_ = false;
};

} // namespace iotd
