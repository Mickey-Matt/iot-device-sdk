// ===========================================================================
// iotd/data_source.h
//
// 【这是整个项目最重要的一课】
//
// 问题：数据可能来自真实串口，也可能来自模拟器
//       如果 DeviceClient 里写死"读串口"，那没有硬件时程序完全没法测
//
// 解决：把"从哪读数据"抽象成一个接口，DeviceClient 只认接口，不认实现
//
//           IDataSource  (接口 / 抽象基类)
//                 ▲
//        ┌────────┴────────┐
//   MockSource      SerialPortSource
//
// 【注意这里没有 MqttSource】
//   MQTT 是数据的【去向】，不是来源 —— 云平台不会凭空产生传感器读数
//   它的抽象在另一个头文件：iotd/telemetry_sink.h
//   一条流水线的两头各有一个接口，这才是完整的形状：
//
//     IDataSource ──▶ DeviceClient ──▶ ITelemetrySink
//      （从哪来）       解析/校验/贴标签       （往哪去）
//
// 这个技巧叫"面向接口编程"或"依赖倒置"
// 好处有三个，你在面试里可以这样讲：
//   1. 可测试：没有硬件也能用 Mock 跑通全流程
//   2. 可替换：换串口/换上报方式，DeviceClient 一行代码不用改
//   3. 可并行：一个人写串口实现，另一个人写 MQTT 实现，互不阻塞
//
// 关键词：virtual（虚函数）、= 0（纯虚函数）、override（覆盖）
// 这三个词是 C++ 多态的核心，也是这个岗位面试的必考点
// ===========================================================================

#pragma once

#include <string>

namespace iotd {

// 一次读取的结果
struct ReadResult {
    bool has_data = false; // 这次有没有读到数据
    std::string frame;     // 读到的一整帧（已经去掉行尾换行符）
    std::string error;     // 出错信息，空字符串表示没错
};

class IDataSource {
public:
    // 虚析构函数 —— 这一行绝对不能省！
    //
    // 原因：当我们通过基类指针 delete 一个派生类对象时，
    //   如果基类析构函数不是 virtual，就只会调用基类的析构函数，
    //   派生类自己的清理代码（比如关闭串口句柄）不会被执行 → 资源泄漏
    //
    // 面试必考："为什么基类析构函数要声明为 virtual？"
    // 标准答法：为了保证通过基类指针删除派生类对象时，
    //           派生类的析构函数能被正确调用，避免资源泄漏
    virtual ~IDataSource() = default;

    // = 0 表示"纯虚函数"：这个接口不提供实现，派生类必须自己实现
    // 含有纯虚函数的类叫"抽象类"，不能被直接实例化 ——
    // 你写 IDataSource src; 编译器会直接报错，这正是我们想要的约束
    virtual bool Open(std::string& error) = 0;
    virtual void Close() = 0;

    // 非阻塞读一帧：有数据就返回，没数据就立刻返回（has_data=false）
    // 为什么不设计成"阻塞直到有数据"？因为那样程序就没法优雅退出了
    virtual ReadResult Poll() = 0;

    // 给人类看的名字，日志和报错时用
    virtual std::string Name() const = 0;
};

} // namespace iotd
