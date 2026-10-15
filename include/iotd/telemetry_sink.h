// ===========================================================================
// iotd/telemetry_sink.h
//
// 【这是 data_source.h 的镜像】
//
//   IDataSource      回答"数据从哪来"  → MockSource / SerialPortSource
//   ITelemetrySink   回答"数据往哪去"  → 上云的 MqttSink、只打日志的 LogSink
//
// 为什么要做成两个接口，而不是给 IDataSource 加一个 Publish()？
//   传感器（模拟器、串口）只会【产生】数据，你没法"往传感器里发数据"，
//   硬加一个 Publish() 它们只能写空实现骗编译器 —— 那是污染接口
//   分开之后，能读的只管读，能写的只管写，谁也不用为不存在的能力负责
//
// 【为什么 MqttSink 既发遥测又收命令，却只继承这一个接口？】
//   因为一条 MQTT 连接本来就是双向的
//   但"收命令"不走这个接口 —— 它是 MqttSink 自己的虚函数 OnCommand，
//   和 IDataSource / ITelemetrySink 都无关，属于另一条链路
//
// 一个类同时实现两个正交接口是常规做法，C++ 标准库自己就这么干：
//   std::basic_iostream : public basic_istream<>, public basic_ostream<>
//   —— 一个对象、两个接口、共用同一份底层缓冲区（见 istream 头文件 996 行）
// 本项目同理：一个 MqttSink 对象、两个方向、共用同一条 socket
// ===========================================================================

#pragma once

#include <string>

#include "iotd/types.h"

namespace iotd {

class ITelemetrySink {
public:
    // 虚析构函数：通过接口指针销毁实现类时，保证实现类的析构能跑
    // 理由和 IDataSource 那条完全一样
    virtual ~ITelemetrySink() = default;

    // 打开出口：MqttSink 在这里连 broker，LogSink 什么都不用做
    // 为什么出口也要 Open/Close，而不是构造完就能用？
    //   因为"连上"是一件可能失败、也可能中途断开的事，
    //   把它做成显式的一步，失败才能报给调用方（构造函数不方便报错）
    virtual bool Open(std::string& error) = 0;
    virtual void Close() = 0;

    // 把一条已经打包好的消息送出去
    // 返回 false 时 error 里写明原因（未连接 / 报文过大 / 队列满）
    //
    // 参数为什么是 DeviceMessage 而不是 SensorReading？
    //   因为要发出去的是"一条完整的消息"，而不是"一个读数" ——
    //   只收读数就会把 device_id / 时间戳 / 序号丢在半路，
    //   这个坑真实发生过，见 docs/踩坑记录.md 第 14 条
    virtual bool Publish(const DeviceMessage& message, std::string& error) = 0;
};

} // namespace iotd
