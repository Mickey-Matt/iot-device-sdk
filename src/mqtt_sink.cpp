// ===========================================================================
// src/mqtt_sink.cpp  —— libmosquitto 的接入实现
// ===========================================================================

#include "iotd/mqtt_sink.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "iotd/json.h"
#include "iotd/logger.h"

namespace iotd {
namespace {

// ---------------------------------------------------------------------------
// mosquitto_lib_init() 是全库一次性初始化，规则：
//   - 进程里只能 init 一次，退出时 cleanup 一次
//   - 官方文档明确警告它【不是线程安全】的
// 所以用 std::call_once 保证：无论多少个 MqttSink 对象、多少线程，
// 初始化代码只执行一次，这是 C++11 提供的标准做法
// ---------------------------------------------------------------------------
std::once_flag g_lib_init_flag;
bool g_lib_init_ok = false;

void EnsureLibraryInitialized() {
    std::call_once(g_lib_init_flag, []() {
        const int rc = ::mosquitto_lib_init();
        g_lib_init_ok = (rc == MOSQ_ERR_SUCCESS);
        if (!g_lib_init_ok) {
            LOG_ERROR("mosquitto_lib_init 失败：" << ::mosquitto_strerror(rc));
        }
    });
}

// 生成一个唯一的客户端 ID
// MQTT 规定：如果两个客户端撞 ID 并连同一个 broker，
// 后连上的会把先连上的【踢下线】，造成类似"设备反复掉线"的问题，
std::string MakeClientId(const std::string& prefix) {
    using namespace std::chrono;
    const auto us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    return prefix + "-" + std::to_string(static_cast<long long>(us));
}

} // namespace

// ---------------------------------------------------------------------------
// 构造与析构
// ---------------------------------------------------------------------------

MqttSink::MqttSink(Config config) : config_(std::move(config)) {
    if (config_.client_id.empty()) {
        config_.client_id = MakeClientId("iotd");
    }
}

MqttSink::~MqttSink() {
    // 析构里关闭连接 —— RAII 思想：对象生命周期结束，资源自动释放
    // 即使调用方忘了调 Close()，也不会泄漏连接和 socket
    Close();
}

std::string MqttSink::UpTopic() const {
    return BuildUpTopic(config_.device_id);
}

std::string MqttSink::CommandTopic() const {
    return BuildCmdTopic(config_.device_id);
}

// ---------------------------------------------------------------------------
// Open：建立连接 + 挂载回调
// ---------------------------------------------------------------------------
bool MqttSink::Open(std::string& error) {
    if (mosq_ != nullptr) {
        error = "MqttSink 已经打开过了";
        return false;
    }

    EnsureLibraryInitialized();
    if (!g_lib_init_ok) {
        error = "mosquitto 库初始化失败";
        return false;
    }

    // 每次打开都重置"首次连接"的状态，避免复用对象时残留上次的结果
    connect_attempt_done_ = false;
    connect_succeeded_ = false;

    // --- 第 1 步：创建客户端实例 ---
    // 参数说明：
    //   id           客户端唯一标识
    //   clean_session
    //     true  = 不保留会话状态，重连后要重新订阅
    //     false = broker 帮你记住订阅关系和未确认消息（复杂，适合弱网设备）
    //   obj          用户数据指针，回调函数通过它拿回 this
    mosq_ = ::mosquitto_new(config_.client_id.c_str(), /*clean_session=*/true, this);
    if (mosq_ == nullptr) {
        error = "mosquitto_new 失败（内存不足或客户端 ID 非法）";
        return false;
    }

    // --- 第 2 步：挂回调 ---
    ::mosquitto_connect_callback_set(mosq_, &MqttSink::OnConnectStatic);
    ::mosquitto_disconnect_callback_set(mosq_, &MqttSink::OnDisconnectStatic);
    ::mosquitto_message_callback_set(mosq_, &MqttSink::OnMessageStatic);

    // --- 第 3 步：鉴权（可选）---
    if (!config_.username.empty()) {
        ::mosquitto_username_pw_set(mosq_, config_.username.c_str(),
                                    config_.password.empty() ? nullptr : config_.password.c_str());
        // 注意这里传的是 config_ 里 std::string 的指针
        // mosquitto 内部会拷贝它，所以 config_ 生命周期 >= mosq_ 就没问题
        // 我们的 config_ 是成员变量，天然满足
    }

    // --- 第 4 步：配置自动重连退避 ---
    ::mosquitto_reconnect_delay_set(mosq_, static_cast<unsigned int>(config_.reconnect_delay_sec),
                                    static_cast<unsigned int>(config_.reconnect_delay_max_sec),
                                    /*reconnect_exponential_backoff=*/true);

    // --- 第 5 步：发起异步连接 ---
    // connect_async 和 loop_start 必须成对
    // connect_async 只负责"把请求写进客户端代办队列"
    // 真正去连、去收发、去重连的，是 loop_start 后台起的那个循环
    ++stats_.connect_attempts;
    LOG_INFO("正在连接 MQTT broker " << config_.broker_host << ":" << config_.broker_port
                                     << " client_id=" << config_.client_id
                                     << " keepalive=" << config_.keepalive_sec << "s");

    const int rc = ::mosquitto_connect_async(mosq_, config_.broker_host.c_str(),
                                             config_.broker_port, config_.keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        error = std::string("连接请求未能入队：") + ::mosquitto_strerror(rc);
        ::mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    // --- 第 6 步：启动后台网络线程 ---
    // 【这是自动重连的关键】
    // mosquitto_loop() 没有自动重连功能
    // 只有 loop_start / loop_forever 才有内建的重连逻辑
    const int loop_rc = ::mosquitto_loop_start(mosq_);
    if (loop_rc != MOSQ_ERR_SUCCESS) {
        error = std::string("启动网络线程失败：") + ::mosquitto_strerror(loop_rc);
        ::mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    // --- 第 7 步：等首次连接结果（最多 3 秒）---
    // 使用条件变量等待后台线程的唤醒（HandleConnect 中的.notify_all）
    {
        std::unique_lock<std::mutex> lock(connect_mutex_);
        connect_cv_.wait_for(lock, std::chrono::seconds(3),
                             [this] { return connect_attempt_done_; });

        if (!connect_succeeded_) {
            error = "连接 broker " + config_.broker_host + ":" +
                    std::to_string(config_.broker_port) + " 超时或失败。\n" +
                    "  请确认 broker 已启动：mosquitto -p " + std::to_string(config_.broker_port);
            ::mosquitto_loop_stop(mosq_, /*force=*/true);
            ::mosquitto_destroy(mosq_);
            mosq_ = nullptr;
            return false;
        }
    }

    LOG_INFO("MQTT 初始化完成，后台网络线程已启动（自动重连已启用）");
    return true;
}

//---------------------------------------------------------------------------
// Close：断开并释放
//---------------------------------------------------------------------------
void MqttSink::Close() {
    if (mosq_ == nullptr) {
        return;
    }

    // 【顺序很重要，这三步不能随便换】
    //
    // 1) 必须先 disconnect，再 loop_stop —— 反过来会卡十几秒
    //    后台线程可能正阻塞在 select() 或重连等待里，join 只能干等它自己醒过来
    //    disconnect 会主动唤醒后台线程，让它立刻看到停止标志
    //    实测（连接正常时）：
    //      先 loop_stop 再 disconnect → 13011 ms
    //      先 disconnect 再 loop_stop → 0 ms
    //
    // 2) disconnect 不能省：不发 DISCONNECT 报文的话，broker 会认为
    //    你是"异常掉线"，可能要等 keepalive 超时才释放客户端 ID
    //
    // 3) destroy 必须放在 loop_stop 之后：线程还在跑时 destroy 会让它
    //    访问已释放的内存 → 崩溃
    //
    // --- force 到底做了什么（读源码 + 实测更正，原来的因果写错了）---
    // 这里以前写的是"force=true 本该关掉 socket 把 poll 打断，但 Windows 上不生效"，
    // 实际 mosquitto_loop_stop 内部是两件独立的事：
    //   1) send(sockpairW, 1 字节) —— 打断 select()，和 force 无关，总是执行
    //   2) if(force) pthread_cancel(thread_id) —— force 唯一的动作，硬杀线程
    // 也就是：负责"叫醒"的是 1)，force 管的是"叫不醒就直接杀掉"
    //
    // 实测（本机环境，没有 broker 在跑）：
    //   - 1) 的唤醒字节不可靠，force=false 时 6 次全部卡满 15 秒，一次都没被叫醒
    //   - force=true 时能否快退取决于 pthread_cancel 赶上的时机，
    //     随机两态：约 3 秒（cancel 及时）或约 15 秒（没赶上）
    //   - 原文说"连不上 broker 时反而 0 ms"也不对：恰恰是连不上时
    //     （线程在重连循环里，卡在阻塞 connect 或退避等待上）更容易卡住
    //     所以别把 force 当成"保证立刻返回"的手段
    //
    if (connected_.load()) {
        ::mosquitto_disconnect(mosq_);
        connected_ = false;
    }

    ::mosquitto_loop_stop(mosq_, /*force=*/true);
    ::mosquitto_destroy(mosq_);
    mosq_ = nullptr;

    Stats snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = stats_;
    }
    LOG_INFO("MQTT 已断开：" << config_.broker_host << ":" << config_.broker_port
                             << "（统计：连接成功 " << snapshot.connect_success << " 次，收到 "
                             << snapshot.messages_received << " 条，发布成功 "
                             << snapshot.publish_ok << " 条，发布失败 " << snapshot.publish_failed
                             << " 条）");
}

//----------------------------------------------------------------------------
// 上行发布
//----------------------------------------------------------------------------
bool MqttSink::Publish(const DeviceMessage& message, std::string& error) {

    // 手工拼一条 JSON 报文
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  R"({"device_id":"%s","timestamp_ms":%lld,"uptime_ms":%lld,"seq":%u,)"
                  R"("temp":%.2f,"humid":%.2f,"light_lux":%d})",
                  config_.device_id.c_str(), static_cast<long long>(message.timestamp_ms),
                  static_cast<long long>(message.uptime_ms), message.sequence,
                  message.reading.temperature_c, message.reading.humidity_pct,
                  message.reading.light_lux);

    if (!PublishRaw(UpTopic(), buf, error)) {
        return false;
    }

    // 同步打印日志
    LOG_INFO("↑ 上报成功 #" << message.sequence << " temp=" << message.reading.temperature_c
                            << "C humid=" << message.reading.humidity_pct << "% "
                            << message.reading.light_lux << "lux 开机" << message.uptime_ms
                            << "ms");
    return true;
}

bool MqttSink::PublishRaw(const std::string& topic, const std::string& payload,
                          std::string& error) {
    if (mosq_ == nullptr) {
        error = "MqttSink 尚未打开";
        ++stats_.publish_failed;
        return false;
    }
    if (!connected_.load()) {
        // 未连接时直接失败，而不是排进队列等重连
        // （如果业务需要保证不丢，应该用 clean_session=false 让 broker 暂存，
        //   或者自己在本地做持久化队列 —— 这是可以展开讲的架构取舍）
        error = "尚未连接到 broker，无法发布";
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.publish_failed;
        return false;
    }

    // mosquitto_publish 参数：
    //   mid         消息 ID 输出（用于追踪确认，这里不需要，传 nullptr）
    //   topic       主题
    //   payloadlen  载荷长度（注意是字节数，不是字符数）
    //   payload     载荷指针
    //   qos         服务质量
    //   retain      是否保留（true 时 broker 会存住最后一条，新订阅者立刻收到）
    const int rc =
        ::mosquitto_publish(mosq_, /*mid=*/nullptr, topic.c_str(), static_cast<int>(payload.size()),
                            payload.c_str(), config_.qos, /*retain=*/false);
    if (rc != MOSQ_ERR_SUCCESS) {
        error = std::string("发布到 ") + topic + " 失败：" + ::mosquitto_strerror(rc);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.publish_failed;
        }
        LOG_WARN(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.publish_ok;
    }
    LOG_DEBUG("已发布到 " << topic << "（" << payload.size() << " 字节）");
    return true;
}

// ---------------------------------------------------------------------------
// 回调的实现
// ---------------------------------------------------------------------------

//  C 库只认普通函数指针，不认 C++ 成员函数指针
// 所以每个回调都写成"静态壳子 + 转调成员函数"
void MqttSink::OnConnectStatic(struct mosquitto* /*mosq*/, void* obj, int rc) {
    static_cast<MqttSink*>(obj)->HandleConnect(rc);
}

void MqttSink::OnDisconnectStatic(struct mosquitto* /*mosq*/, void* obj, int rc) {
    static_cast<MqttSink*>(obj)->HandleDisconnect(rc);
}

void MqttSink::OnMessageStatic(struct mosquitto* /*mosq*/, void* obj,
                               const struct mosquitto_message* msg) {
    if (msg == nullptr || msg->topic == nullptr) {
        return;
    }
    // 【注意】msg->payload 不一定以 '\0' 结尾
    // MQTT 载荷是二进制安全的，长度由 payloadlen 给出
    // 如果直接当 C 字符串用（std::string(payload)），
    // 轻则读到垃圾字符，重则越界读取导致崩溃
    // 正确做法：用 (指针, 长度) 构造 std::string
    const std::string topic(msg->topic);
    const std::string payload(static_cast<const char*>(msg->payload),
                              static_cast<std::size_t>(msg->payloadlen));
    static_cast<MqttSink*>(obj)->HandleMessage(topic, payload);
}

// ---------------------------------------------------------------------------
// 连接结果回调
// ---------------------------------------------------------------------------
void MqttSink::HandleConnect(int rc) {
    if (rc != 0) {
        // rc 是 broker 返回的 CONNACK 结果码，含义要和网络错误区分开：
        //   1 = 协议版本不被支持
        //   2 = 客户端 ID 被拒绝
        //   3 = 服务不可用
        //   4 = 用户名或密码错误     ← 现场最常见
        //   5 = 未授权
        static const char* kReason[] = {"成功",       "协议版本不支持",   "客户端 ID 被拒绝",
                                        "服务不可用", "用户名或密码错误", "未授权"};
        const char* reason = (rc >= 0 && rc <= 5) ? kReason[rc] : "未知原因";
        LOG_ERROR("broker 拒绝连接（码 " << rc << "：" << reason << "）");
        connected_ = false;
        // 连接失败就唤醒在 Open() 里等待的主线程，别干等 3 秒
        {
            std::lock_guard<std::mutex> lock(connect_mutex_);
            connect_attempt_done_ = true;
            connect_succeeded_ = false;
        }
        connect_cv_.notify_all();
        return;
    }

    connected_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.connect_success;
    }
    LOG_INFO("已连接到 broker：" << config_.broker_host << ":" << config_.broker_port << " (QoS "
                                 << config_.qos << ")（第 " << GetStats().connect_success
                                 << " 次成功）");

    // 唤醒 Open() 里等待的主线程
    {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        connect_attempt_done_ = true;
        connect_succeeded_ = true;
    }
    connect_cv_.notify_all();

    // -----------------------------------------------------------------------
    // 【注意】订阅必须放在连接成功回调里，不能放在 Open() 里
    //
    // 因为：Open() 里 mosquitto_connect() 只是发出请求，
    // 那时连接还没建立、会话还没生效，此时订阅可能失败或不生效
    // 而断线重连后，连接回调会再次被触发 —— 订阅也就自动恢复了
    // 这是很多人写 MQTT 客户端时"重连后收不到消息"的根因
    // -----------------------------------------------------------------------
    const std::string topic = CommandTopic();
    const int sub_rc = ::mosquitto_subscribe(mosq_, /*mid=*/nullptr, topic.c_str(), config_.qos);
    if (sub_rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("订阅 " << topic << " 失败：" << ::mosquitto_strerror(sub_rc));
    } else {
        // 订阅时也指定 QoS 1：这表示"我能接受 broker 用 QoS 1 发给我"
        // 注意发布端和订阅端的 QoS 可以不同，
        // 实际生效的 QoS = min(发布端QoS, 订阅端QoS)，面试常考这个
        LOG_INFO("已订阅下行主题：" << topic << "（QoS " << config_.qos << "）");
    }
}

void MqttSink::HandleDisconnect(int rc) {
    connected_ = false;
    if (rc == 0) {
        LOG_INFO("与 broker 的连接已正常关闭");
    } else {
        // 非 0 表示异常断开，mosquitto 内部会按我们在 Open() 里
        // 设置的退避策略自动重连 —— 不需要我们手动写重试循环
        LOG_WARN("与 broker 的连接异常断开（码 " << rc << "），将按退避策略自动重连");
    }
}

// ---------------------------------------------------------------------------
// 收到下行消息：立即调基类虚函数或子类实现
// ---------------------------------------------------------------------------
void MqttSink::HandleMessage(const std::string& topic, const std::string& payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.messages_received;
    }
    LOG_INFO("收到下行消息 topic=" << topic << " payload=" << payload);

    // 交给虚函数，让子类立即响应命令
    //
    // 【注意】这里调用 OnCommand 时不能持有 mutex_
    // 因为子类的 OnCommand 里可能会调 PublishRaw()，
    // 而 PublishRaw() 也要拿同一把锁 → 就会自己等自己，形成死锁
    // 这是多线程编程里最经典的一类错误，务必记住：
    //   "不要在持锁期间调用外部代码（回调、虚函数）"
    OnCommand(topic, payload);
}

void MqttSink::OnCommand(const std::string& topic, const std::string& payload) {

    // 基类默认实现：只打日志
    LOG_INFO("[默认命令处理] " << topic << " -> " << payload);
}

} // namespace iotd
