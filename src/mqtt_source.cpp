// ===========================================================================
// src/mqtt_source.cpp  —— libmosquitto 的接入实现
//
// 阅读顺序建议：
//   1. 看 Open()   —— 连接是怎么建立的、回调是怎么挂上的
//   2. 看 Poll()   —— 网络 I/O 和消息投递是怎么驱动的
//   3. 看 HandleConnect/HandleDisconnect —— 自动重连的原理
//   4. 最后看 PublishTelemetry —— 上行发布
// ===========================================================================

#include "iotd/mqtt_source.h"

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
// 所以用 std::call_once 保证：无论多少个 MqttSource 对象、多少线程，
// 初始化代码只执行一次，这是 C++11 提供的标准做法
//
// 注意：我们用单线程 loop 方案，其实天然不会并发调用；
// 但写上 call_once 是"防御性编程"——将来有人改成多线程也不会炸
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

// 生成一个大概率唯一的客户端 ID
// 为什么需要唯一？MQTT 规定：如果两个客户端用同一个 ID 连同一个 broker，
// 后连上的会把先连上的【踢下线】，现场那种"设备反复掉线"的诡异问题，
// 十有八九就是客户端 ID 撞了
std::string MakeClientId(const std::string& prefix) {
    using namespace std::chrono;
    const auto us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    return prefix + "-" + std::to_string(static_cast<long long>(us));
}

} // namespace

// ===========================================================================
// 构造与析构
// ===========================================================================

MqttSource::MqttSource(Config config) : config_(std::move(config)) {
    if (config_.client_id.empty()) {
        config_.client_id = MakeClientId("iotd");
    }
}

MqttSource::~MqttSource() {
    // 析构里关闭连接 —— RAII 思想：对象生命周期结束，资源自动释放
    // 即使调用方忘了调 Close()，也不会泄漏连接和 socket
    Close();
}

std::string MqttSource::Name() const {
    return "MqttSource(" + config_.broker_host + ":" + std::to_string(config_.broker_port) + ")";
}

std::string MqttSource::UpTopic() const {
    return BuildUpTopic(config_.device_id);
}

std::string MqttSource::CommandTopic() const {
    return BuildCmdTopic(config_.device_id);
}

// ===========================================================================
// Open：建立连接 + 挂载回调
// ===========================================================================
bool MqttSource::Open(std::string& error) {
    if (mosq_ != nullptr) {
        error = "MqttSource 已经打开过了";
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
    //   id           客户端唯一标识（见 MakeClientId 的注释，撞 ID 会被踢）
    //   clean_session
    //     true  = 不保留会话状态，重连后要重新订阅（简单，适合我们）
    //     false = broker 帮你记住订阅关系和未确认消息（复杂，适合弱网设备）
    //   这里选 true 教学优先；真实弱网设备常用 false + 固定 client_id
    //   obj          用户数据指针，回调里通过它拿回 this
    mosq_ = ::mosquitto_new(config_.client_id.c_str(), /*clean_session=*/true, this);
    if (mosq_ == nullptr) {
        error = "mosquitto_new 失败（内存不足或客户端 ID 非法）";
        return false;
    }

    // --- 第 2 步：挂回调 ---
    // 注意传的是 this —— 回调里再把它转回 MqttSource*
    ::mosquitto_connect_callback_set(mosq_, &MqttSource::OnConnectStatic);
    ::mosquitto_disconnect_callback_set(mosq_, &MqttSource::OnDisconnectStatic);
    ::mosquitto_message_callback_set(mosq_, &MqttSource::OnMessageStatic);

    // --- 第 3 步：鉴权（可选）---
    if (!config_.username.empty()) {
        ::mosquitto_username_pw_set(mosq_, config_.username.c_str(),
                                    config_.password.empty() ? nullptr : config_.password.c_str());
        // 注意这里传的是 config_ 里 std::string 的指针
        // mosquitto 内部会拷贝它，所以 config_ 生命周期 >= mosq_ 就没问题
        // —— 我们的 config_ 是成员变量，天然满足
    }

    // --- 第 4 步：配置自动重连退避 ---
    // 这是"断线后能自己恢复"的关键，也是面试里的加分项
    //
    // 【单位是秒】文档原文："reconnect_delay - the number of seconds to wait
    // between reconnects." 传毫秒数值进去会变成天文数字的等待
    ::mosquitto_reconnect_delay_set(mosq_, static_cast<unsigned int>(config_.reconnect_delay_sec),
                                    static_cast<unsigned int>(config_.reconnect_delay_max_sec),
                                    /*reconnect_exponential_backoff=*/true);

    // --- 第 5 步：发起异步连接 ---
    // 为什么用 connect_async 而不是 connect？
    // 因为连接结果要等 broker 回 CONNACK，而我们会用后台线程跑网络循环
    // async 版本只负责"把请求发出去"，结果由连接回调告诉我们
    ++stats_.connect_attempts;
    LOG_INFO("正在连接 MQTT broker " << config_.broker_host << ":" << config_.broker_port
                                     << " client_id=" << config_.client_id);

    const int rc = ::mosquitto_connect_async(mosq_, config_.broker_host.c_str(),
                                             config_.broker_port, config_.keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        error = std::string("发起连接失败：") + ::mosquitto_strerror(rc);
        ::mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    // --- 第 6 步：启动后台网络线程 ---
    // 【这一行是自动重连的关键】
    // 前面踩过的坑：手动调用 mosquitto_loop() 不会触发自动重连，
    // 只有 loop_start / loop_forever 才有内建的重连逻辑
    const int loop_rc = ::mosquitto_loop_start(mosq_);
    if (loop_rc != MOSQ_ERR_SUCCESS) {
        error = std::string("启动网络线程失败：") + ::mosquitto_strerror(loop_rc);
        ::mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    // --- 第 7 步：等首次连接结果（最多 3 秒）---
    // 为什么要等？因为这是一个命令行工具，
    // 如果 broker 没启动，用户应该【立刻】看到明确的错误提示，
    // 而不是程序"看起来启动了"却什么都发不出去
    // 这里用条件变量而不是 sleep：连接一成功就立刻返回，不浪费 3 秒
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

// ===========================================================================
// Close：断开并释放
// ===========================================================================
void MqttSource::Close() {
    if (mosq_ == nullptr) {
        return;
    }

    // 【顺序很重要，这三步不能随便换】
    //
    // 1) 必须先 disconnect，再 loop_stop —— 反过来会卡十几秒
    //    后台线程平时阻塞在 poll() 上，而 poll 的超时是按 keepalive 算的
    //    （keepalive_sec = 15 → 线程一睡就是十几秒），force=true 本该关掉
    //    socket 把 poll 打断，但它在 Windows 上不生效（libmosquitto issue #117），
    //    于是 loop_stop 里的 join 只能干等线程自己醒过来
    //    disconnect 会主动唤醒线程，让它立刻看到停止标志
    //    实测（libmosquitto 2.0.22 + MSYS2 UCRT64，keepalive=15，连接正常时）：
    //      先 loop_stop 再 disconnect → 13011 ms
    //      先 disconnect 再 loop_stop → 0 ms
    //    这个坑只在【连接正常】时发作，连不上 broker 时线程在重连等待里，
    //    force 恰好打得断，反而是 0 ms —— 所以它才这么难查
    //
    // 2) destroy 必须放在 loop_stop 之后：线程还在跑时 destroy 会让它
    //    访问已释放的内存 → 崩溃
    //
    // 3) disconnect 也不能省：不发 DISCONNECT 报文的话，broker 会认为
    //    你是"异常掉线"，可能要等 keepalive 超时才释放客户端 ID
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
    LOG_INFO("MQTT 已断开：" << Name() << "（统计：连接成功 " << snapshot.connect_success
                             << " 次，收到 " << snapshot.messages_received << " 条，发布成功 "
                             << snapshot.publish_ok << " 条，发布失败 " << snapshot.publish_failed
                             << " 条）");
}

// ===========================================================================
// Poll：驱动网络 + 投递一帧
// ===========================================================================
ReadResult MqttSource::Poll() {
    ReadResult result;

    if (mosq_ == nullptr) {
        result.error = "MqttSource 尚未打开";
        return result;
    }

    // -----------------------------------------------------------------------
    // 驱动网络？不需要了 —— 这件事已经交给后台线程
    //
    // mosquitto_loop_start() 创建的线程在持续做三件事：
    //   1. 收发网络报文（包括 QoS 1 的 PUBACK 确认）
    //   2. 维持 keepalive 心跳
    //   3. 【自动重连】—— 这是它的核心价值，手动 loop() 没有这个能力
    //
    // 所以 Poll() 现在只剩一件事：把队列里的数据取出来
    // 【教训】换了线程模型之后，原来那段 mosquitto_loop() 调用必须删掉，
    // 否则会和后台线程争抢同一个 socket，属于未定义行为
    // -----------------------------------------------------------------------
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_frames_.empty()) {
        result.has_data = true;
        result.frame = std::move(pending_frames_.front());
        pending_frames_.erase(pending_frames_.begin());
    }

    return result;
}

// ===========================================================================
// 上行发布
// ===========================================================================
bool MqttSource::PublishTelemetry(const DeviceMessage& message, std::string& error) {
    // 手工拼一条 JSON 报文
    // 这里刻意不用 json::Dump —— 因为要演示"设备侧怎么产生报文"，
    // 而且真实嵌入式设备也是这么一位一位拼出来的（省内存）
    //
    // 字段来源的分工：device_id 取自 config_（它同时决定主题），
    // timestamp_ms / uptime_ms / seq 只能取自 message ——
    // 这三项是"每条消息各不相同"的属性，由 DeviceClient 在生成消息时填好，
    // 在这里重新取一次时间戳就等于有两个时间来源，日志和报文会对不上
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  R"({"device_id":"%s","timestamp_ms":%lld,"uptime_ms":%lld,"seq":%u,)"
                  R"("temp":%.2f,"humid":%.2f,"light_lux":%d})",
                  config_.device_id.c_str(), static_cast<long long>(message.timestamp_ms),
                  static_cast<long long>(message.uptime_ms), message.sequence,
                  message.reading.temperature_c, message.reading.humidity_pct,
                  message.reading.light_lux);
    return Publish(UpTopic(), buf, error);
}

bool MqttSource::Publish(const std::string& topic, const std::string& payload, std::string& error) {
    if (mosq_ == nullptr) {
        error = "MqttSource 尚未打开";
        ++stats_.publish_failed;
        return false;
    }
    if (!connected_.load()) {
        // 未连接时直接失败，而不是排进队列等重连
        // 为什么？因为设备遥测数据"过期就没价值了"，
        // 断网 30 秒后补发 30 秒前的温度没有意义
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
    //   payload     载荷指针（二进制安全，所以必须给长度）
    //   qos         服务质量等级
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

// ===========================================================================
// 回调的实现
// ===========================================================================

// 静态转发的意义：C 库只认普通函数指针，不认 C++ 成员函数指针
// 所以每个回调都写成"静态壳子 + 转调成员函数"
void MqttSource::OnConnectStatic(struct mosquitto* /*mosq*/, void* obj, int rc) {
    static_cast<MqttSource*>(obj)->HandleConnect(rc);
}

void MqttSource::OnDisconnectStatic(struct mosquitto* /*mosq*/, void* obj, int rc) {
    static_cast<MqttSource*>(obj)->HandleDisconnect(rc);
}

void MqttSource::OnMessageStatic(struct mosquitto* /*mosq*/, void* obj,
                                 const struct mosquitto_message* msg) {
    if (msg == nullptr || msg->topic == nullptr) {
        return;
    }
    // 【极易踩的坑】msg->payload 不一定以 '\0' 结尾！
    // MQTT 载荷是二进制安全的，长度由 payloadlen 给出
    // 如果直接当 C 字符串用（std::string(payload)），
    // 轻则读到垃圾字符，重则越界读取导致崩溃
    // 正确做法：用 (指针, 长度) 构造 std::string
    const std::string topic(msg->topic);
    const std::string payload(static_cast<const char*>(msg->payload),
                              static_cast<std::size_t>(msg->payloadlen));
    static_cast<MqttSource*>(obj)->HandleMessage(topic, payload);
}

// ---------------------------------------------------------------------------
// 连接结果回调
// ---------------------------------------------------------------------------
void MqttSource::HandleConnect(int rc) {
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
        // 唤醒可能在 Open() 里等待首次连接结果的主线程，别再干等 3 秒
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
    LOG_INFO("已连接到 broker：" << config_.broker_host << ":" << config_.broker_port << "（第 "
                                 << GetStats().connect_success << " 次成功）");

    // 唤醒 Open() 里等待的主线程
    {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        connect_attempt_done_ = true;
        connect_succeeded_ = true;
    }
    connect_cv_.notify_all();

    // -----------------------------------------------------------------------
    // 【重要】订阅必须放在连接成功回调里，不能放在 Open() 里！
    //
    // 因为：Open() 里 mosquitto_connect() 只是发出请求，
    // 那时连接还没建立、会话还没生效，此时订阅可能失败或不生效
    // 而断线重连后，连接回调会【再次】被触发 —— 订阅也就自动恢复了
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

void MqttSource::HandleDisconnect(int rc) {
    connected_ = false;
    if (rc == 0) {
        LOG_INFO("与 broker 的连接已正常关闭");
    } else {
        // 非 0 表示异常断开，此时 mosquitto 内部会按我们在 Open() 里
        // 设置的退避策略自动重连 —— 不需要我们手动写重试循环
        // 这就是"用库"和"自己造"的区别：自带的能力要先用上
        LOG_WARN("与 broker 的连接异常断开（码 " << rc << "），将按退避策略自动重连");
    }
}

// ---------------------------------------------------------------------------
// 收到下行消息：投递到队列，等 Poll() 取走
// ---------------------------------------------------------------------------
void MqttSource::HandleMessage(const std::string& topic, const std::string& payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.messages_received;
    }
    LOG_INFO("收到下行消息 topic=" << topic << " payload=" << payload);

    // 先交给虚函数，让子类可以【立即】响应命令
    // 顺序很重要：命令处理应该先于入队，因为命令是"事件"，要求尽快执行
    //
    // 【注意】这里调用 OnCommand 时【不能】持有 mutex_！
    // 因为子类的 OnCommand 里可能会调 Publish()，
    // 而 Publish() 也要拿同一把锁 → 就会自己等自己，形成死锁
    // 这是多线程编程里最经典的一类错误，务必记住：
    //   "不要在持锁期间调用外部代码（回调、虚函数）"
    OnCommand(topic, payload);

    if (!config_.queue_downlink) {
        // 默认路径：命令到此为止，不进遥测队列
        //
        // 【为什么这是对的？】
        // 下行命令的语义是"立刻做一件事"，不是"产生一条数据"
        // 如果把它也塞进 IDataSource 的队列，DeviceClient 会拿它当设备遥测去解析，
        // 结果是"报文缺少 device_id 字段"这类毫无意义的解析失败 —— 我们已经踩到了
        // 分清"事件"和"数据流"，是 MQTT 客户端设计里的关键判断
        return;
    }

    // 可选路径：确实需要把下行内容也当作一路数据源时（如推送配置块）
    std::lock_guard<std::mutex> lock(mutex_);
    pending_frames_.push_back(payload);

    // 队列保护：如果上层很久不调用 Poll()，队列会一直涨
    // 设个上限，超过就丢最旧的 —— 宁可丢数据，也不能把内存吃光
    // 这类"兜底"代码是生产级实现和新手 demo 的分水岭
    constexpr std::size_t kMaxPending = 256;
    while (pending_frames_.size() > kMaxPending) {
        pending_frames_.erase(pending_frames_.begin());
        LOG_WARN("待处理队列超过 " << kMaxPending << " 条，已丢弃最旧的一条");
    }
}

void MqttSource::OnCommand(const std::string& topic, const std::string& payload) {
    // 默认实现：只打日志
    // 真实业务里这里会是"解析命令 → 操作硬件 → 回执"
    // 打开这个日志是为了让你能直观看到"下行链路是通的"
    LOG_INFO("[默认命令处理] " << topic << " -> " << payload);
}

} // namespace iotd
