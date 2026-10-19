# iotd — 物联网设备接入 SDK 与调试工具

一套用于**设备数据接入与链路排障**的 C++17 工具集：通过统一的设备客户端接口采集数据、解析 JSON 报文、输出结构化日志，并支持串口与模拟数据源互换

> **项目起点**：为定位"设备数据丢失、解析失败"这类现场问题而开发
> **当前状态**：v0.2 — 数据源抽象 + 串口/模拟/MQTT 三种数据源 + 报文解析 + 结构化日志 + 排障统计 + 断线自动重连

---

## 这个项目解决什么问题

设备在现场出问题时，最常见的三个疑问是：

1. 数据到底有没有发出来？
2. 如果发出来了，是在哪一段丢的？
3. 收到的字节为什么解析不出来？

`iotd` 的答案是：**把链路上每一段都变成可观测的**

- 收到多少帧、解析成功多少帧、失败多少帧，实时统计
- 解析失败的帧**原样打印**，而不是只说一句"解析失败"
- JSON 解析错误带**字符位置**，直接指出是第几个字节出的问题
- 输出到终端的同时落盘日志文件，便于事后回溯

---

## 快速开始

### 三步跑起来

```bash
# 在 iot-device-sdk 目录下
cmake -B build -G Ninja -S .     # 生成构建文件（第一次跑一次即可）
cmake --build build              # 编译（每次改完代码跑这个）

# 另开一个窗口启动 broker，保持不关
mosquitto -p 1883 -v

# 回到项目窗口运行
./build/mqtt_demo.exe
```

> 详细的环境验证步骤（7 关，含每关的失败排查）见 `../学习路线/01-环境通关清单.md`

### 三个核心命令

```bash
# 1. 用模拟数据源跑通全流程（不需要任何硬件）
./build/iot-debug.exe mock 500

# 2. 查看本机可用串口
./build/iot-debug.exe list

# 3. 读取真实串口（115200 波特率）
./build/iot-debug.exe serial COM3 115200
```

**没有硬件也能完整跑通**——这是刻意的设计：数据源被抽象成接口，模拟器和真实串口可以互换，因此开发、测试、演示都不依赖硬件

### 复现现场问题：先录，再放

```bash
# 让客户（或你自己）开着录制跑一遍，文件里存的是串口上真实跑过的字节块
./build/iot-debug.exe serial COM3 115200 --record field.cap

# 拿回来按原来的时间间隔回放，走的是和串口完全相同的分帧代码
./build/iot-debug.exe replay field.cap

# 内置五个现场病例，不用硬件也能看到半包、粘包、乱码、超限、CRLF 各自长什么样
./build/iot-debug.exe replay samples/captures/half.txt
./build/iot-debug.exe replay samples/captures/oversize.txt
```

录制文件一行一条记录：`@毫秒<TAB>字节块`，`#` 开头是注释

**为什么录的是字节块而不是帧**：只有原始字节块才留着"这次只收到 12 字节"这类信息，退化成整帧之后，半包和粘包就再也测不出来了

### 验证 JSON 解析能力

```bash
# 从文件解析
./build/iot-debug.exe parse --file samples/good_message.json

# 直接传字符串时必须用单引号：JSON 里用双引号
./build/iot-debug.exe parse '{"device_id":"fish-tank-01","temp":25.6}'

# 故意解析一个坏报文，看错误定位
./build/iot-debug.exe parse --file samples/broken_message.json
```

输出示例（坏报文）：

```
解析失败：位置 69：对象的键必须是字符串
定位：
  {"device_id":"fish-tank-01","timestamp_ms":1790000000000,"temp":25.6,
                                                                       ^
```


### 运行测试

```bash
cd build
ctest --output-on-failure
# 或直接运行
./iotd_tests.exe        # JSON 解析与报文校验
./iotd_framer_tests.exe # 分帧、录制格式、五个现场病例的回放
```

---

## 目录结构与职责

```text
iot-device-sdk/
├─ CMakeLists.txt              构建脚本
├─ include/iotd/               对外头文件（SDK 的门面）
│   ├─ types.h                 公共数据结构：SensorReading / DeviceMessage
│   ├─ json.h                  JSON 解析器接口
│   ├─ logger.h                日志器
│   ├─ data_source.h           数据源抽象接口（数据从哪来）
│   ├─ telemetry_sink.h        上报出口抽象接口（数据往哪去）
│   ├─ line_framer.h           分帧：字节流切成一条条完整消息
│   ├─ capture.h               录制文件格式（读、写、转义）
│   ├─ capture_recorder.h      把串口收到的字节块录成文件
│   ├─ file_replay_source.h    按录制时的时间线回放字节块
│   ├─ mock_source.h           模拟数据源
│   ├─ serial_port_source.h    串口数据源（Windows API）
│   ├─ mqtt_sink.h             MQTT 出口：上报遥测 + 接收命令
│   ├─ log_sink.h              只打日志的出口（不联网）
│   └─ device_client.h         设备客户端：SDK 主入口
├─ src/                        实现（使用者不需要看）
├─ examples/device_sim.cpp     示例程序：模拟设备持续上报
├─ examples/mqtt_demo.cpp      示例程序：完整的网关（采集 → 上报 → 收命令）
├─ tools/iot_debug_main.cpp    调试工具：命令行多子命令
├─ tests/test_json.cpp         单元测试：JSON 解析与报文校验
├─ tests/test_framer.cpp       单元测试：分帧、录制、回放
├─ samples/                    示例报文（含正常与损坏两种）
│   └─ captures/               五个现场病例的录制文件
└─ docs/
    ├─ 实测记录.md              跑出来的真实日志和抓包结果
    └─ 踩坑记录.md              踩过的坑 + 排查过程 + 取舍理由
```

`iotd_core` 只放生产代码；`MockSource` 和 `FileReplaySource` 在独立的 `iotd_test_support` 里，
客户链接 SDK 时不会带上一堆只有开发和排障才用得上的东西

**为什么头文件和实现分开？**

`include/` 是对外承诺，`src/` 是内部实现，使用者只需要读 `include/` 就能用这个 SDK，不必关心 `json.cpp` 里那 300 行解析逻辑是怎么写的，这也是为什么修改实现不会导致大规模重新编译

---

## 架构：两头都可替换

```text
    IDataSource  (抽象接口)                        ITelemetrySink  (抽象接口)
          |                                                |
  +-------+--------+                        +--------------+--------------+
MockSource   SerialPortSource            MqttSink                     LogSink
(无硬件自测 /  (真实串口，          (接入 broker，上报到云)    (只打日志，不联网)
  测试注入)      Windows API)
          |                                                |
          +-------------------+                +-----------+
                              v                |
                         DeviceClient ---------+
                  (取一帧 -> 解析 -> 校验 -> 贴标签 -> 交给出口)
```

**设计要点**：`DeviceClient` 只认两个接口，不知道数据从哪来，也不知道数据往哪去，因此：

- **换数据源**：模拟器 / 真实串口 / 录制回放，`DeviceClient` 一行代码不用改
- **换上报方式**：打进日志 ↔ 发到云端，`DeviceClient` 一行代码不用改
- 单元测试可以用 Mock 注入半包、乱码、超长帧等边界数据
- 没有硬件、没有 broker 也能开发和演示

**上下游各一个接口，是对称的**：`IDataSource` 回答"从哪读"，`ITelemetrySink` 回答"往哪发"

一个真实的网关就是把它们接起来（见 `examples/mqtt_demo.cpp`）：

```cpp
iotd::MqttSink   cloud(config);      // 出口：上报到云 + 接收命令
iotd::MockSource sensor(3000);       // 来源：模拟传感器（换 SerialPortSource 就是真硬件）
iotd::DeviceClient client(sensor, cloud);

while (!g_should_exit) {
    client.PollOnce();               // 整条上行链路就这一行
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
```

**命令下行不走 `DeviceClient`** —— 命令是"事件"（立刻做一件事），遥测是"数据流"（攒起来往上发），两者语义不同，所以命令从 broker 直接进 `MqttSink::OnCommand` 虚函数，由你的子类处理

---

## 已实现的排障能力

| 能力 | 实现方式 | 对应现场问题 |
|---|---|---|
| 串口粘包/半包 | 按换行符分帧 + 缓冲区（上限 8KB 保护） | "数据偶尔缺一段" |
| 现场问题复现 | 录原始字节块 → 按原时序回放，走同一条分帧代码 | "客户说数据不对，我这边复现不出来" |
| 坏帧不致命 | 解析失败只记录并跳过，不退出进程 | "一帧坏数据导致服务挂掉" |
| 错误精确定位 | JSON 解析器报告出错字符位置 | "到底哪个字节有问题" |
| 原始内容留证 | 坏帧原样打印，不只报"解析失败" | "事后无法复现" |
| 数据合理性校验 | 温度 -40~80℃、湿度 0~100% 边界检查 | "传感器故障导致的脏数据" |
| 链路健康度 | 帧数/成功数/失败数统计 | "客户说老掉线，先看哪个指标" |
| BOM 兼容 | 自动剥离 UTF-8 BOM | "记事本存的文件解析不了" |

---

## Roadmap

- [x] **M1** 数据源抽象 + 模拟源 + 串口源 + JSON 解析 + 日志 + 测试
- [x] **M2** 接入 MQTT（libmosquitto）：上行发布、下行订阅、QoS 1、指数退避自动重连、命令回执
- [x] **M2.5** 上报出口抽象（`ITelemetrySink`）：把 MQTT 从"数据源"改回"出口"，入站出站对称
- [x] **M2.6** 分帧抽成 `LineFramer`：串口与录制回放共用；录制/回放走同一个字节块格式
- [ ] **M3** 补齐 5 个示例程序（publish / subscribe / reconnect / serial_bridge / multi_device）
- [ ] **M4** 文档：设计说明（DESIGN.md）、FAQ、排障案例集（含 Wireshark 抓包与日志证据）

### M2 实测状态（2026-10-01）

| 能力 | 状态 | 证据 |
|---|---|---|
| 连接 broker / 订阅下行主题 | ✅ 已验证 | 日志 `已连接到 broker（第 1 次成功）` + `已订阅下行主题` |
| 上行遥测上报（QoS 1） | ✅ 已验证 | 日志 `↑ 上报成功 temp=24.5C humid=61%` |
| 下行命令接收与分发 | ✅ 已验证 | 收到 `{"cmd":"feed"}` → `>>> 执行喂食` → 回执到 `cmd_ack` |
| **断线自动重连** | ✅ 已验证 | 杀掉 broker → 客户端不退出 → broker 重启后 `已连接到 broker（第 2 次成功）` |
| **重连后订阅自动恢复** | ✅ 已验证 | 重连后再次收到下行命令并正确执行 |
| 坏报文健壮性 | ✅ 已验证 | 非法 JSON 命令被拒绝并打印原始内容，进程不崩 |

> 实现过程中踩到并修复了三个问题（缺 DLL 导致启动即退出、手动 loop 无重连能力、日志非线程安全），
> 完整的排查方法记录在 `docs/踩坑记录.md`

---

## 构建环境

| 项 | 版本 |
|---|---|
| 语言标准 | C++17 |
| 构建系统 | CMake ≥ 3.20 + Ninja（实测 CMake 4.4.3） |
| 编译器 | **MSYS2 UCRT64（GCC 16.2.0）**，官方推荐环境 |
| MQTT 库 | **libmosquitto 2.0.22**（MSYS2 pacman 包，动态链接） |
| 其他外部依赖 | **无**（JSON 解析器与日志器为项目内自实现） |

> **构建环境选择说明**（核实自 [MSYS2 官方文档](https://www.msys2.org/docs/environments/)）：
> MSYS2 的默认环境是 **UCRT64**；**MINGW64 已于 2026-03-15 被官方弃用**（不再接收新包）
> UCRT64 有现成的 `mingw-w64-ucrt-x86_64-mosquitto` pacman 包，这是选它的直接好处
> （`paho.mqtt.c` 在 MSYS2 **没有**包，其官方预编译包是 MSVC 编译的、不能链进 MinGW 程序）
> 环境搭建过程见 `docs/实测记录.md`

> **运行提示**：程序动态链接 libmosquitto，产物目录已自动复制 6 个依赖 DLL
> （libmosquitto / libcrypto / libssl / libwinpthread / libstdc++ / libgcc）
> 缺少它们会以退出码 `-1073741511`（`0xC0000139`）静默退出，见 `docs/踩坑记录.md` 第 1 节

> 依赖版本与核实日期记录在此，避免半年后照着旧教程配置失败
> 最后核实：2026-10-01

---

## 为什么第一版不用现成的库？

M1 阶段刻意不引入 nlohmann/json、spdlog，原因有三：

1. **教学**：手写一遍解析器，才能真正理解分帧、转义、类型系统这些问题
2. **零依赖**：环境配置失败是新手放弃项目的头号原因
3. **可讲述**：面试时能讲清"我为什么这样设计一个解析器"，比"我会用一个库"有说服力

**M2 阶段则相反——MQTT 直接用成熟的 libmosquitto**，因为协议实现涉及网络、心跳、重连、QoS 握手，
自己造既无必要也容易出错，**什么时候该用轮子，什么时候该自己造，这个对比本身就是很好的工程判断素材**
