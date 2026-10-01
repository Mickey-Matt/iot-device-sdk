# iotd 项目学习手册 · 第二期：MQTT 接入

> 配套代码：`D:\Matt\JobHunt\iot-device-sdk\`
> 状态：**已用 MSYS2 UCRT64 + libmosquitto 2.0.22 实测通过**（连接 / 上报 / 下行命令 / 断线自动重连 / 订阅自动恢复，全部验证）
> 阅读建议：先看第 3 节的三个坑（那是本文最有价值的部分），再回来看第 2 节的代码。

---

## 1. 第二期做成了什么

### 1.1 一句话

**把 MQTT 接进来，让设备能上报数据、能接收云端命令、断线能自己恢复——而且核心代码一行没改。**

这不是夸张。第二期新增的只有：

```
include/iotd/mqtt_source.h   ← 新增（约 200 行，含注释）
src/mqtt_source.cpp          ← 新增（约 450 行，含注释）
examples/mqtt_demo.cpp       ← 新增（示例程序）
CMakeLists.txt               ← 改了 4 处（找库、链接、加示例、复制 DLL）
```

**`device_client.h`、`device_client.cpp`、`json.cpp`、`types.h` 一个字都没动。**

这就是第一期把数据源抽象成 `IDataSource` 接口的回报：

```
                     IDataSource
            ┌──────────────┼──────────────┐
      MockSource   SerialPortSource   MqttSource   ← 第二期只是加了第三个实现
```

`DeviceClient` 至今不知道数据是从模拟器、串口还是 MQTT 来的。**依赖倒置的价值在这一期兑现了。**

### 1.2 实测日志（真实输出，不是示例）

```
2026-10-01 16:32:09.484 [INFO ] 已连接到 broker：localhost:1883（第 1 次成功）
2026-10-01 16:32:09.484 [INFO ] 已订阅下行主题：dev/fish-tank-01/cmd（QoS 1）
2026-10-01 16:32:12.645 [INFO ] ↑ 上报成功 temp=24.5C humid=61%
2026-10-01 16:32:14.477 [INFO ] 收到下行消息 topic=dev/fish-tank-01/cmd payload={"cmd":"feed","amount":2}
2026-10-01 16:32:14.478 [INFO ] >>> 执行喂食：投喂 2 克（这里本该驱动步进电机）
2026-10-01 16:32:14.478 [INFO ] 已回执到 dev/fish-tank-01/cmd_ack
2026-10-01 16:32:16.505 [WARN ] 与 broker 的连接异常断开（码 7），将按退避策略自动重连
2026-10-01 16:32:18.937 [WARN ] ↑ 上报失败：尚未连接到 broker，无法发布
2026-10-01 16:32:28.675 [INFO ] 已连接到 broker：localhost:1883（第 2 次成功）   ← 自动重连成功
2026-10-01 16:32:28.675 [INFO ] 已订阅下行主题：dev/fish-tank-01/cmd（QoS 1）   ← 订阅自动恢复
2026-10-01 16:32:43.566 [INFO ] 收到下行消息 topic=dev/fish-tank-01/cmd payload={"cmd":"light_on"}
2026-10-01 16:32:43.567 [INFO ] >>> 开启补光（这里本该拉高 PWM）
```

**把中间那 10 秒的断线期留着不删，比一个"完美的日志"更有说服力**——面试时你能指着它讲清楚整条恢复链路。

---

## 2. 先跑通（10 分钟）

### 2.1 前提

你已经装好 MSYS2 UCRT64 + cmake + ninja + mosquitto（第一期第 2.2 节）。验证一下：

```bash
which g++ && cmake --version | head -1 && ls /ucrt64/include/mosquitto.h
```

### 2.2 三个窗口的操作顺序

**窗口 A —— 启动 broker（保持运行）：**

```bash
mosquitto -p 1883 -v
```

`-v` 会打印每一条报文收发，**排查时非常有用**：能看到客户端到底有没有连上来、有没有订阅成功。
（注意 mosquitto 2.x 默认只监听 localhost，本机自测不用改配置；要让局域网其他机器连需要写 `listener 1883` + `allow_anonymous true` 并指定 `-c` 配置文件。）

**窗口 B —— 编译并运行我们的程序：**

```bash
cd /d/Matt/JobHunt/iot-device-sdk
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja
./mqtt_demo.exe localhost 1883
```

**窗口 C —— 模拟云端下发命令：**

```bash
# 用 -f 从文件发（推荐！命令行传 JSON 会被 shell 吃掉双引号）
mosquitto_pub -h localhost -t dev/fish-tank-01/cmd -f /d/Matt/JobHunt/iot-device-sdk/samples/cmd_feed.json

# 看设备有没有发上来数据
mosquitto_sub -h localhost -t 'dev/fish-tank-01/up' -v
```

**窗口 B 里应该立刻出现** `>>> 执行喂食：投喂 2 克`。

### 2.3 想验证断线重连？照这个做

1. 程序正常运行中，**在窗口 A 按 Ctrl+C 杀掉 broker**
2. 窗口 B 会出现：`与 broker 的连接异常断开（码 7），将按退避策略自动重连`
3. 再过几秒：`↑ 上报失败：尚未连接到 broker，无法发布`（**这是正常的**，断网期间的数据确实发不出去）
4. **重新启动 broker**：`mosquitto -p 1883 -v`
5. 窗口 B 应该在几秒内出现：`已连接到 broker（第 2 次成功）` + `已订阅下行主题`
6. 再发一次命令，设备照样能收到

**这 6 步做完，你就有了一个真实的"断线自愈"演示。** 面试时这是硬通货。

---

## 3. ⭐ 三个真实的坑（本文最值钱的部分）

这三个都是我在实测中**真的踩到**的，不是从教程抄的。重点不是结论，而是**诊断方法**——以后你遇到任何库的问题都能用。

### 坑 1：程序一启动就退出，没有任何输出

**现象**：`mqtt_demo.exe` 双击/运行时瞬间退出，日志文件都没生成，退出码 `-1073741511`。

**诊断过程**：

**第 1 步：把退出码转成十六进制**

```
-1073741511  →  0xC0000139
```

**为什么要转十六进制？** 因为 Windows 的错误码在十六进制下才有意义。`0xC0000139` 是 `STATUS_ENTRYPOINT_NOT_FOUND`，含义是"某个 DLL 的一个函数入口点找不到"。**看到 `0xC0000xxx` 开头，基本可以直接怀疑动态链接库问题。**

**第 2 步：用 objdump 查真实的 DLL 依赖**

```bash
objdump -p libmosquitto.dll | grep "DLL Name"
```

结果发现 `libmosquitto.dll` 还依赖：

```
libcrypto-3-x64.dll     ← OpenSSL 加密库
libssl-3-x64.dll        ← OpenSSL TLS 库
libwinpthread-1.dll     ← MinGW 线程库
```

而 `mqtt_demo.exe` 自己依赖 `libstdc++-6.dll`、`libgcc_s_seh-1.dll`。

**我最初只往 build 目录复制了 `libmosquitto.dll` 一个文件**——依赖不全，加载器直接放弃。

**第 3 步：核心认知——依赖是"传递"的**

```
mqtt_demo.exe
  └─ libmosquitto.dll
       ├─ libcrypto-3-x64.dll
       ├─ libssl-3-x64.dll
       └─ libwinpthread-1.dll
```

**A 依赖 B，B 又依赖 C 和 D。只复制直接依赖必然出事。**

**修法**：CMakeLists 里改成批量复制 6 个 DLL（见 CMakeLists 末尾那段，注释里写了完整排查过程）。同时也加了 `-static-libgcc -static-libstdc++`，把 GCC 自己的运行库静态链进 exe，少两个依赖。

**你能从这个坑学到什么**：
- `0xC0000139` / `0xC000007b` → 查 DLL
- `objdump -p xxx.dll | grep "DLL Name"` 是查依赖的标准手段
- 交付程序时，"能在我机器上跑"不等于"能在别人机器上跑"——**这就是为什么很多软件装完要带一堆 DLL**

---

### 坑 2：MQTT 断线后永远不重连（这个最严重）

**现象**：broker 挂掉后，程序不但不重连，还每 20 毫秒往日志里刷一条错误，几秒钟就刷了几百条：

```
[ERROR] 数据源错误：MQTT 连接异常：The client is not currently connected.
[ERROR] 数据源错误：MQTT 连接异常：The client is not currently connected.
...（刷屏，且 broker 重启后依然不恢复）
```

**根因**：我一开始用的是**手动驱动网络循环**的方案：

```cpp
// 我最初的（错误）设计
const int rc = mosquitto_loop(mosq_, 0, 1);   // 在主循环里每 20ms 调一次
```

理由听起来很合理："回调都在同一个线程执行，完全不用加锁，对新手友好。"

**但 `mosquitto_loop()` 没有自动重连能力。**

查官方头文件 `mosquitto.h` 第 1207-1216 行才看到这段：

> Section: Network loop (managed by libmosquitto)
> The two recommended approaches are to use either **mosquitto_loop_forever** or **mosquitto_loop_start**. ...
> **Use this function [loop_start] when you have other tasks you need to run at the same time as the MQTT client, e.g. reading data from a sensor.**

以及第 1227 行（关于 loop_forever）：

> **It handles reconnecting in case server connection is lost.**

**自动重连是 `loop_forever` / `loop_start` 提供的能力，手动调 `loop()` 没有。** 而且文档明确说了：你还有别的任务要跑（我们有主循环和传感器数据）→ 就该用 `loop_start`。

**修法**（三处改动）：

```cpp
// 1. Open() 里：连接改用 async，然后启动后台网络线程
mosquitto_connect_async(mosq_, host, port, keepalive);
mosquitto_loop_start(mosq_);        // ← 这一行是自动重连的关键

// 2. Poll() 里：删掉那段 mosquitto_loop() 调用！
//    因为后台线程已经在驱动网络，再手动调会和它争抢同一个 socket（未定义行为）

// 3. 所有共享数据加锁（详见坑 3）
```

**加一个体验优化**：`Open()` 里用条件变量等首次连接结果（最多 3 秒）。这样 broker 没启动时，用户**立刻**看到明确错误，而不是程序"看起来启动了"却什么都发不出去。

**你能从这个坑学到什么**：
- **"看起来合理的设计"不等于"库支持的设计"。** 我为了"避免多线程"选了单线程方案，结果牺牲了核心功能
- **遇到库的行为不符合预期，第一件事是读它的头文件/官方文档**，而不是猜。我查了 `mosquitto.h` 才发现段落标题就写着 "Network loop (managed by libmosquitto)"，官方早就指明了推荐用法
- 代价必须付：用 `loop_start` 就得加锁，**不能为了省事牺牲正确性**

---

### 坑 3：加了后台线程之后，日志开始重复

**现象**：修好坑 2 之后，日志里出现了重复行：

```
[INFO ] DeviceClient 已启动，数据源：MqttSource(localhost:1883)
[INFO ] DeviceClient 已启动，数据源：MqttSource(localhost:1883)    ← 重复！
[INFO ] 已订阅下行主题：dev/fish-tank-01/cmd（QoS 1）
[INFO ] 已订阅下行主题：dev/fish-tank-01/cmd（QoS 1）              ← 重复！
```

**诊断过程（值得学）**：

**第 1 步：先排除"是不是上次测试的日志残留"**

```powershell
# 检查文件的创建时间，以及里面有没有旧时间戳的行
Get-Item mqtt_demo.log | Select-Object CreationTime
Get-Content mqtt_demo.log | Select-String '16:29|16:31'    # 旧测试的时间
```

结果：文件创建时间就是本次测试开始时刻，没有旧行 → **不是残留，是真 bug**。

**第 2 步：确认代码里是不是写了两遍**

```bash
grep -n "mosquitto_subscribe\|已订阅下行主题" src/mqtt_source.cpp
```

结果：只有一处调用 → **不是代码重复，是同一段代码被执行了两次**。

**第 3 步：得出真正结论**

`std::ofstream` 和 `std::cerr` **都不是线程安全的**。现在有两个线程：

- 主线程：调用 `LOG_INFO("DeviceClient 已启动")`
- MQTT 后台线程：连接回调里调用 `LOG_INFO("已订阅下行主题")`

两个线程同时写同一个流，**流内部的缓冲区被交错破坏**，症状就是某行内容被打印两次或错乱。

**修法**：给 `Logger` 加锁。

```cpp
// logger.h
mutable std::mutex mutex_;   // mutable 必需：Log 是 const 语义但加锁要改 mutex

// logger.cpp
void Logger::Log(Level level, const std::string& message) {
    ...
    std::lock_guard<std::mutex> lock(mutex_);   // 整个输出过程原子完成
    std::cerr << line << '\n';
    if (file_.is_open()) { file_ << line << '\n'; file_.flush(); }
}
```

改完后重测：**19 行日志，0 重复行。**

**你能从这个坑学到什么**：
- **一旦程序里有了第二个线程，"日志"这种看起来最无害的东西也会出问题。** 这就是多线程的可怕之处：bug 出现在你想不到的地方
- 诊断要**先排除最简单的可能**（残留文件），再逐步深入
- `std::cerr` 你以为它安全？它只是"没有缓冲"，**不等于"线程安全"**

---

## 4. 代码讲解

### 4.1 文件总览

| 文件 | 行数 | 职责 |
|---|---|---|
| `include/iotd/mqtt_source.h` | ~200 | 配置、统计、接口声明 |
| `src/mqtt_source.cpp` | ~450 | 连接/重连/订阅/发布/回调 全部实现 |
| `examples/mqtt_demo.cpp` | ~180 | 示例：上报 + 命令处理 + 统计 |
| `CMakeLists.txt` | +40 行 | 找 libmosquitto、链接、复制 DLL |

### 4.2 头文件里三个关键设计

**(1) `MqttConfig` 为什么定义在类外面？**

因为踩了一个编译错误：如果 Config 是嵌套类，那 `MqttSource` 的构造函数不能写 `MqttSource(Config c = Config{})`——GCC 16 会报：

```
error: default member initializer for 'broker_host' required before the end of
       its enclosing class
```

它要求在 `MqttSource` 类还没定义完之前就使用 `Config` 的默认成员初始化器，这是不允许的。把 `Config` 提到命名空间层级就没问题，类里用 `using Config = MqttConfig;` 保留 `MqttSource::Config` 这种写法。

**(2) `connected_` 用 `atomic<bool>`，`stats_` 用 `mutex`——为什么不一样？**

```cpp
std::atomic<bool> connected_{false};
mutable std::mutex mutex_;      // 保护 stats_ 和 pending_frames_
Stats stats_;
```

- `connected_` 只是一个 bool 标志，`atomic` 就够，且不会阻塞
- `stats_` 是 5 个计数器的结构体，必须保证"读到的是一组一致的快照"，所以用锁

**按数据特点选同步手段**，不是无脑全用锁或全用 atomic。

**(3) `GetStats()` 返回快照而不是引用**

```cpp
Stats GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;          // 拷贝一份返回
}
```

原来的版本返回 `const Stats&`，那是单线程时代的设计。现在统计计数会被后台线程改，**返回引用会让调用方读到"改了一半"的数据**。

### 4.3 实现里四个必看的地方

**(1) `Open()` 的七步流程**

```
① mosquitto_lib_init()     全库初始化（用 call_once 保证只执行一次）
② mosquitto_new()          创建客户端（client_id 撞了会被踢下线）
③ 挂三个回调               连接/断开/收消息
④ reconnect_delay_set()    配置退避（★ 单位是秒！）
⑤ connect_async()          异步发起连接
⑥ loop_start()             ★ 启动后台线程 = 自动重连能力
⑦ 等首次连接结果            最多 3 秒，成功就跑，失败就报明确错误
```

**(2) `Poll()` 现在只做一件事**

```cpp
std::lock_guard<std::mutex> lock(mutex_);
if (!pending_frames_.empty()) { ... 取一帧 ... }
```

网络 I/O 已经交给后台线程，**不再需要手动 `mosquitto_loop()`**。这一段注释里写了完整的原因。

**(3) 订阅必须放在连接回调里，不能放在 `Open()`**

```cpp
void MqttSource::HandleConnect(int rc) {
    ...
    const std::string topic = CommandTopic();
    mosquitto_subscribe(mosq_, nullptr, topic.c_str(), config_.qos);
}
```

**为什么？** 因为 `connect()` 只是"请求已发出"，那时会话还没建立，订阅可能不生效。
更重要的是：**断线重连后连接回调会再次触发，订阅就自动恢复了。**

> 这是很多人写 MQTT 客户端时"重连后收不到消息"的根因。实测日志里你能看到它确实work：
> `已连接到 broker（第 2 次成功）` 紧接着就是 `已订阅下行主题`。

**(4) 回调里不能持锁调用虚函数（死锁陷阱）**

```cpp
void MqttSource::HandleMessage(const std::string& topic, const std::string& payload) {
    { std::lock_guard<std::mutex> lock(mutex_); ++stats_.messages_received; }
    LOG_INFO("收到下行消息 ...");

    OnCommand(topic, payload);      // ← 在【不持锁】的状态下调用

    if (!config_.queue_downlink) return;

    std::lock_guard<std::mutex> lock(mutex_);   // 这里才加锁入队
    pending_frames_.push_back(payload);
}
```

**为什么 `OnCommand` 必须在锁外面调用？** 因为子类的 `OnCommand` 里可能调用 `Publish()`，而 `Publish()` 也要拿同一把锁 → **自己等自己，死锁**。

这是多线程最经典的一类错误，记成一句话：**"不要在持锁期间调用外部代码（回调、虚函数）。"**

### 4.4 一个设计取舍：命令不进遥测队列

实测中发现下行命令会被 `DeviceClient` 当成遥测数据解析一次，产生无意义的报错：

```
[INFO ] >>> 执行喂食：投喂 2 克
[WARN ] 丢弃一帧坏数据：报文缺少 device_id 字段 | 原始内容: {"cmd":"feed","amount":2}
```

**根因**：命令走了和遥测同一条队列。

**修法**：加 `config.queue_downlink`（默认 `false`）——

> **命令是"事件"，要求立即执行；遥测是"数据流"，可以排队处理。**
> 把两者混在一起是设计错误。分清它们，是 MQTT 客户端设计的关键判断。

---

## 5. 面试怎么讲这一期

### 5.1 三个能撑起 20 分钟对话的点

**① "你怎么保证设备断网后能自己恢复？"**

> "两层保障。第一层是 MQTT 自带的 keepalive 心跳和 QoS 1 确认，能发现链路异常；第二层是重连退避——首次等 1 秒，失败翻倍，最大 30 秒。
>
> 我在这上面踩过一个坑：一开始我用手动 `mosquitto_loop()` 驱动网络，觉得单线程不用加锁更简单，结果实测发现**它根本不重连**——broker 重启后客户端躺着不动。查官方头文件才明白，自动重连是 `loop_forever`/`loop_start` 的能力。改成 `loop_start` 后正常了，代价是回调跑到后台线程，所以我给队列、统计、连接标志都加了锁。
>
> 另外退避参数还有个单位坑：文档写的是秒，我按毫秒传了 1000，等于要等 17 分钟才重连。"

**这段话里有设计、有踩坑、有诊断过程、有取舍——比背概念强太多。**

**② "你的日志系统怎么设计的？"**

> "三要素：时间戳（用来和 broker 日志、抓包对齐）、级别（平时 INFO，排查看 DEBUG）、出错的原始内容。
>
> 特别说一下最后一条：解析失败时我会把**原始帧原样打出来**，而不是只打一句'解析失败'。因为事后复现不了的时候，那行原始内容就是唯一的证据。
>
> 还有一次教训：加了 MQTT 后台线程后日志出现重复行，排查发现 `std::ofstream` 和 `std::cerr` 都不是线程安全的，两个线程同时写会破坏流缓冲区。后来给日志加了锁。"

**③ "你怎么判断问题出在哪一段链路？"**

> "用分段指标。设备侧有帧数、解析成功数、失败数；MQTT 侧有连接尝试次数、连接成功次数、发布成功/失败数。
>
> 举个例子：如果连接成功次数是 1 且不再增长，但发布成功也不涨 → 根本没连上；如果连接成功次数是 3 → 中间发生过两次断线重连，说明网络不稳；如果发布失败有值 → 断网期间的数据丢了多少条，一目了然。
>
> 这套思路是从运维那边借鉴的：**不要猜，用指标分段定位。**"

### 5.2 一句话项目介绍（更新版）

> "我用 C++17 写了一套物联网设备接入 SDK 和调试工具。数据源做了接口抽象，所以模拟器、串口、MQTT 可以互换——接 MQTT 的时候核心解析和调度代码一行都没改。
>
> MQTT 这块我用 libmosquitto 实现了上行发布、下行订阅、QoS 1、心跳保活和指数退避重连，实测过杀掉 broker 再重启，客户端能在几秒内自动恢复并重新订阅。
>
> 它主要解决现场排障：解析失败的帧会原样打印并带字符位置定位，链路健康度用连接次数、收发条数、失败数几个指标分段判断。"

---

## 6. 第二期验收清单

- [ ] `ninja` 编译通过，`build/` 里能看到 `mqtt_demo.exe` 和 6 个 DLL
- [ ] `mosquitto -p 1883 -v` 起 broker，`./mqtt_demo.exe localhost 1883` 能连上
- [ ] 用 `mosquitto_sub -t 'dev/fish-tank-01/up' -v` 能看到设备上报的数据
- [ ] 用 `mosquitto_pub ... -f samples/cmd_feed.json` 能让程序打印 `>>> 执行喂食`
- [ ] **杀掉 broker → 重启 → 程序自动重连，且重连后还能收到命令**（最重要的验收项）
- [ ] 日志里没有重复行
- [ ] 能用 5.2 那段话介绍这个项目

---

## 7. 下一步（第三期建议）

按 JD 的要求，还缺这些：

| 优先级 | 要做的事 | 对应 JD 要求 |
|---|---|---|
| **P0** | **`docs/DESIGN.md`**：把本文第 4 节的设计决策整理成一份"接口设计说明" | "整理技术文档" |
| **P0** | **`docs/FAQ.md`**：把三个坑写成"问题—原因—解决"三段式 | "归纳常见问题并形成标准处理流程" |
| **P1** | 补齐示例程序：`02_subscribe.cpp`、`03_reconnect.cpp`、`04_serial_bridge.cpp` | "完成 SDK 及示例程序的开发与交付" |
| **P1** | 用 `DeviceClient` 同时接 MQTT + 串口（一路从设备读，一路往云端发，做真正的网关） | 展示架构能力 |
| **P2** | Wireshark 抓一次 MQTT 报文，存截图（能看到 CONNECT/CONNACK/SUBSCRIBE/PUBLISH/PUBACK） | 排障证据链 |

**建议先做 P0 的两份文档**——因为 JD 里"技术文档"和"FAQ 整理"是明确写着的职责，而这两份文档你已经有全部素材了（就是本文第 3 节），改写一下就行。**投入半天，直接对上一个岗位要求。**
