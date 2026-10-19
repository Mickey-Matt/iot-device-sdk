// ===========================================================================
// iotd/mqtt_sink.h
//
// 【第二期核心文件】把 MQTT 接入进来
//
// 它是一个"出口"，不是"数据源"：
//
//      数据源                      DeviceClient                     出口
//   IDataSource  ──── Poll() ────▶ 解析/校验/贴标签 ──── Publish() ────▶  MqttSink
//   MockSource                                                          （本文件）
//   SerialPortSource
//
// 【为什么它不实现 IDataSource？】
//   数据源 = "会产生传感器读数的地方"，模拟器和串口是，MQTT 云平台不是。
//   第一版让它也当一路数据源（用一个 queue_downlink 开关把下行命令塞进队列），
//   结果是：默认配置下这个"数据源"永远产不出数据，挂着牌子不干活。
//   现在把它摘干净了 —— 它只做两件真正相关的事：
//
//     1) 发遥测      DeviceClient 调 Publish()      → PUBLISH dev/{id}/up
//     2) 收命令      broker 推过来 → OnCommand 虚函数 → 你的代码
//
//   这两件事共用同一条 socket，所以留在一个类里（理由见下）
//
// 【为什么必须用 mosquitto_loop_start（后台线程）而不是 mosquitto_loop（单线程）？】
//
// 这是我实际踩过的坑，写下来给你省几个小时：
//
// 我最初用 mosquitto_loop() 在主循环里手动驱动网络，理由听起来很合理——
// "回调都在同一个线程里执行，完全不用加锁，对新手更友好"
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
// 原来【自动重连是 loop_forever / loop_start 提供的能力】，手动调 loop() 没有
// 而且文档明确写了：当你还有别的任务要跑（比如读传感器）时，就该用 loop_start
//
// 所以正确做法是：
//   mosquitto_loop_start()  → 库自己起一个后台线程跑网络循环 + 自动重连
//   我们的主线程           → 只管读传感器、算数据、调 Publish()
//
// 代价：回调（收消息、连接状态变化）现在发生在【后台线程】，
//       而 Publish() 在【主线程】调用 —— 两者会同时访问
//       连接标志和统计计数，所以【必须加锁】
//       这个代价是必须付的，不能为了"避免多线程"而牺牲自动重连
// ===========================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

// 需要 mosquitto.h 才能声明 struct mosquitto * 成员
// 由 CMake 负责把 MSYS2 的 include 目录加进来（见 CMakeLists.txt）
#include <mosquitto.h>

#include "iotd/telemetry_sink.h"
#include "iotd/types.h"

namespace iotd {

// ---------------------------------------------------------------------------
// MqttConfig：MQTT 连接配置
//
// 【为什么真类型在类外面，类里只留一个 using 别名？】
// 这是 STL 的惯例：真类型放命名空间层级，类里给一个嵌套短名。对照 vector：
//   typedef size_t size_type;        → 真类型是 std::size_t
//   typedef _Alloc allocator_type;   → 真类型是 std::allocator<T>
// 于是调用方既能写 std::allocator<int>，也能写 vector<int>::allocator_type
// 工业界的 MQTT SDK 也一样：Paho 的 mqtt::connect_options、AWS 的
// Aws::Iot::MqttClientConnectionConfig 都在命名空间层级，
// 而且它们连嵌套短名都没提供
//
// 判断标准是"这个类型离开外层类还说不说得通"：
//   说得通（自包含的数据包）→ 命名空间层级 + 嵌套别名
//   说不通（生命周期绑定 / 要访问私有成员 / 只是外层类的零件）
//     → 真嵌套类，像 locale::facet、map::value_compare 那样
//
// 放命名空间层级还有个实际好处：可以单独前向声明
//   struct MqttConfig;   ← 只用引用时不必 include 这个头文件
// 嵌套类做不到，写成 "class MqttSink::Config;" 只有警告、不声明任何东西
//
// 顺带绕开了一个 GCC 的已知 bug：如果 Config 是"带默认成员初始化的嵌套类"，
// 又出现在外层类的默认实参里（MqttSink(Config = Config{})），GCC 16 会报
//   "default member initializer for 'broker_host' required before the end of
//    its enclosing class"
// 详见 docs/踩坑记录.md 第 13 条
// ---------------------------------------------------------------------------
struct MqttConfig {
    std::string broker_host = "localhost"; // broker 地址，本机就是 localhost
    int broker_port = 1883;                // 1883 = MQTT 明文端口；8883 = TLS
    std::string client_id;                 // 留空则自动生成唯一 ID
    std::string username;                  // 需要鉴权时填写
    std::string password;
    std::string device_id = "fish-tank-01"; // 用于拼接主题
    int keepalive_sec = 60;                 // 心跳间隔：超时没交互就发 PINGREQ

    // 设备遥测能容忍"偶发重复"（后端去重即可），但不能容忍"丢"，
    // 而 QoS 2 的开销对嵌入式设备不划算，所以 QoS 1 更合适
    int qos = 1;

    // --- 重连退避 ---
    // 断线后先等 1 秒重试；还失败就翻倍（2s、4s…），最多等到 30 秒
    // 为什么必须设上限？想象 broker 挂了 1 小时：
    //   没有退避 → 客户端每秒发起几千次连接，把自己的 CPU 和网络打满
    //   有退避   → 连接尝试频率被压到每分钟 2 次，几乎不耗资源
    // 这就是"重连风暴"的防治手段，也是面试里很能讲的一点
    //
    // 【单位是秒！这是踩过的坑】
    // mosquitto_reconnect_delay_set 的文档参数说明写的是
    // "reconnect_delay - the number of seconds to wait between reconnects"，
    // 我第一版想当然按毫秒传了 1000，结果变成"等 1000 秒再重连"（约 17 分钟），
    // 实测断线后 42 秒都没恢复，改成 1 之后重连立刻正常
    // 教训：凡是"间隔/时长"参数，先确认单位再填数
    int reconnect_delay_sec = 1;
    int reconnect_delay_max_sec = 30;
};

// ---------------------------------------------------------------------------
// MqttSink：数据的【出口】，不是入口
//
// 一、为什么它不实现 IDataSource
//   数据源 = "会产生传感器读数的地方"：模拟器会、串口会、云平台不会
//   第一版让它也当一路数据源，靠一个 queue_downlink 开关把下行命令塞进队列
//   结果是：默认配置下那个"数据源"永远产不出东西，挂着牌子不干活
//   现在摘干净了，它只做两件真正相关的事：
//     1) 发遥测  —— DeviceClient 调 Publish()
//     2) 收命令  —— broker 推过来，走 OnCommand 虚函数
//
// 二、为什么这两件事留在一个类里
//   因为它们共用同一条 socket：
//   MQTT 是一条 TCP 连接上的双向协议，CONNECT 建立会话之后，
//   PUBLISH 和 SUBSCRIBE 都跑在这条会话上，keepalive 和自动重连也只有一份
//   拆成两个对象就得共享同一条 socket，谁负责重连、谁负责析构反而更乱
//   工业界也都是这么做的：libmosquitto 的 mosquitto_connect / _publish /
//   _subscribe 收的是同一个 struct mosquitto *，
//   Paho C++ 的 mqtt::async_client 一个类里既有 publish 也有 subscribe
//
// 三、那"一个类干两件事"不违反单一职责吗
//   不违反 —— 单一职责说的是"一个接口只表达一件事"。
//   这里对外只有一个接口 ITelemetrySink（只管"发"），
//   "收命令"是它自己的虚函数，和接口无关，属于另一条链路
//   对照标准库：std::iostream 也是"一个类 + 两个正交接口（读/写）"
// ---------------------------------------------------------------------------
class MqttSink : public ITelemetrySink {
public:
    using Config = MqttConfig; // 保留 MqttSink::Config 这种写法

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

    explicit MqttSink(Config config = Config{});
    ~MqttSink() override;

    // --- 实现 ITelemetrySink 接口 ---
    // Open 里做两件事：连 broker + 订阅命令主题
    bool Open(std::string& error) override;
    void Close() override;

    // 把一条遥测消息发布到 dev/{device_id}/up
    // 返回 false 时 error 里会写明原因（未连接 / 报文过大 / 队列满）
    //
    // 参数为什么是 DeviceMessage 而不是 SensorReading？
    //   DeviceMessage 里本来就含着一个 SensorReading，只收读数等于主动丢掉
    //   device_id / timestamp_ms / uptime_ms / sequence 四项元数据 ——
    //   这个坑真实发生过，见 docs/踩坑记录.md 第 14 条
    //
    // 字段来源的分工：device_id 取自 config_（它同时决定发布主题），
    //   剩下三项都是"每条消息各不相同"的属性，只能由 DeviceMessage 提供
    bool Publish(const DeviceMessage& message, std::string& error) override;

    // 传输层的原始发布：把任意字符串发到任意主题
    // 【为什么叫 Raw】它跳过"消息 → JSON"这一层封装，直接发字节
    // 用途：调试（手工注入一条命令）、发回执（cmd_ack）
    // 名字带 Raw 是为了和上面那个业务级的 Publish 区分开 ——
    // 两个都叫 Publish 的话，读代码时分不清调用方要的是哪一层
    bool PublishRaw(const std::string& topic, const std::string& payload,
                    std::string& error);

    // 供 device_sim / iot-debug 查询连接状态
    // 【注意】这个标志会被后台线程改写，所以用 std::atomic 而不是 bool
    bool IsConnected() const { return connected_.load(); }

    // 返回统计快照，为什么要"快照"而不是返回引用？
    // 因为统计计数会被后台线程修改，直接返回引用会让调用方读到"改了一半"的数据
    Stats GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    // -----------------------------------------------------------------------
    // 下行命令回调
    //
    // 命令为什么走虚函数，而不是走 DeviceClient 的解析链路？
    //   因为命令是"事件"（立刻做一件事），遥测是"数据流"（攒起来往上发），
    //   两者的语义完全不同。把命令塞进数据流里，只会得到
    //   "报文缺少 device_id 字段"这种毫无意义的解析失败 —— 我们真踩过
    //
    // 为什么这里用 virtual 而不是 std::function？
    //   命令处理通常需要一整个对象的状态（比如操作 GPIO、查配置），
    //   继承比塞一个 lambda 更自然，而且虚函数在 SDK 里更常见、更易理解
    //   两种都是合法设计，能说清"为什么这里选这个"才是重点
    //
    // 默认实现只打日志，要处理命令就继承并 override
    // -----------------------------------------------------------------------
    virtual void OnCommand(const std::string& topic, const std::string& payload);

protected:
    // 给子类用的工具：往命令主题发一条下行指令
    std::string CommandTopic() const;
    std::string UpTopic() const;

private:
    // 回调必须是静态函数或自由函数（C 库不认 C++ 成员函数指针），
    // 所以用 static + void* 把 this 传进来 —— 这是 C 回调的经典写法
    // C 库在触发回调时，会把 void* obj（通常是 this 指针）原封不动地传回来，
    // 这样就能在静态回调函数里通过 static_cast<MqttSink*>(obj) 重新拿到对象的 this 指针
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
    // 用 mosquitto_loop_start() 后，收消息和连接状态变化的回调都跑在
    // 库自己的后台线程里，而 Publish() 在主线程 —— 两边会同时碰
    // 下面这些数据，所以必须保护
    //
    // 为什么 connected_ 用 atomic 而 stats_ 用 mutex？
    //   connected_ 只是一个 bool 标志，atomic 足够且不会阻塞；
    //   stats_ 是一个结构体（5 个计数器），要保证"读到的是一致的一组值"，
    //   必须用锁，这是"按数据特点选同步手段"的判断，不是随便选的
    // -----------------------------------------------------------------------
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_; // 保护 stats_
    Stats stats_;

    // 供 Open() 等待首次连接结果用：
    // 后台线程连上（或失败）后唤醒主线程，避免主线程死等
    std::mutex connect_mutex_;
    std::condition_variable connect_cv_;
    bool connect_attempt_done_ = false;
    bool connect_succeeded_ = false;
};

} // namespace iotd
