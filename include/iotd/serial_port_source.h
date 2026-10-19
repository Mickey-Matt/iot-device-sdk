// ===========================================================================
// iotd/serial_port_source.h
//
// 真实串口读取，这是全项目唯一一个"平台相关"的文件
//
// 【为什么用 Windows API 而不是第三方库？】
//   - 零依赖：你不用下载任何东西，今天就能编译
//   - 透明：你能看清串口到底是怎么打开的
//   - 面试可讲："我直接调 Win32 的 CreateFile/SetCommState/ReadFile"
//
// 代价：不能跨平台，以后要上 Linux 时，只需要再写一个
//       SerialPortSource_Linux.cpp 实现同一个接口，其余代码一行不用改
//       —— 这就是 IDataSource 接口的价值
// ===========================================================================

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "iotd/data_source.h"
#include "iotd/line_framer.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace iotd {

class SerialPortSource : public IDataSource {
public:
    // port_name 形如 "COM3"（程序内部会自动补上 \\.\ 前缀）
    SerialPortSource(std::string port_name, int baud_rate = 115200);
    ~SerialPortSource() override; // override：明确表示我在实现基类的虚析构

    bool Open(std::string& error) override;
    void Close() override;
    ReadResult Poll() override;
    std::string Name() const override;

    // 静态工具：列出本机可用的串口名，供用户选择
    static std::vector<std::string> ListAvailablePorts();

    // 把串口收到的原始字节块原样回调出去（在分帧之前），录制用
    void SetTap(LineFramer::Tap tap) { framer_.SetTap(std::move(tap)); }

private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
    std::string port_name_;
    int baud_rate_;
    LineFramer framer_; // 半包、粘包的拼接全在这里做，本类只管把字节搬进来
};

} // namespace iotd
