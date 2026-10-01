// ===========================================================================
// src/serial_port_source.cpp
//
// 【本项目技术含量最高的一个文件，也是面试最值得讲的一个】
//
// 核心问题：串口是"字节流"，不是"消息流"。
//   你调用一次 ReadFile，可能只收到半条 JSON，
//   也可能一次收到三条 JSON 拼在一起。
//   前者叫"半包/拆包"，后者叫"粘包" —— 这是串口编程第一大坑。
//
// 解决方案（行业标准做法）：
//   1. 定义一个"帧边界"。本项目用换行符 '\n' 作为一帧的结束。
//      （真实协议里也可能是固定长度、或者带长度字段的头部）
//   2. 把收到的所有字节先追加进一个缓冲区 buffer_
//   3. 每次从缓冲区里找 '\n'：
//        找到了   → 取出它之前的内容作为一整帧，从缓冲区删除这部分
//        没找到   → 说明这帧还没收完，留着等下次数据到达
//   4. 缓冲区设上限：如果一直收不到换行符，超过上限就丢弃并报警，
//      否则恶意或异常数据会把内存撑爆
//
// 这套逻辑在面试里叫"分帧"，能讲清楚它是很强的加分项。
// ===========================================================================

#include "iotd/serial_port_source.h"

#include <algorithm>
#include <cstdio>

#include "iotd/logger.h"

namespace iotd {
namespace {

// 缓冲区上限：8KB。正常一帧 JSON 也就几百字节，超过说明出了异常。
constexpr std::size_t kMaxBufferBytes = 8 * 1024;

}  // namespace

SerialPortSource::SerialPortSource(std::string port_name, int baud_rate)
    : port_name_(std::move(port_name)), baud_rate_(baud_rate) {}

SerialPortSource::~SerialPortSource() {
    // 析构函数里关闭句柄 —— 这是 RAII 的思想：
    // 对象生命周期结束时，它持有的资源自动释放，不需要调用方记得手动关。
    // 即使中途发生异常，析构函数依然会被调用（栈展开），所以不会泄漏。
    Close();
}

std::string SerialPortSource::Name() const {
    return "SerialPort(" + port_name_ + "@" + std::to_string(baud_rate_) + ")";
}

bool SerialPortSource::Open(std::string& error) {
#if defined(_WIN32)
    // -----------------------------------------------------------------------
    // 关键点 1：端口名必须加 "\\." 前缀
    //   "COM3"    → 只能打开 COM1~COM9
    //   "\\.\COM3" → 可以打开任意编号的串口（COM10 以上必须这样写）
    //   在 C++ 字符串里写 "\\.\" 需要转义成 "\\\\.\\"
    // -----------------------------------------------------------------------
    const std::string device_path = "\\\\.\\" + port_name_;

    handle_ = ::CreateFileA(
        device_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,  // 读写权限
        0,                             // 独占打开：串口不能被两个程序同时用
        nullptr,                       // 默认安全属性
        OPEN_EXISTING,                 // 必须已存在，不创建
        0,                             // 同步模式
        nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            error = "打开 " + port_name_ + " 被拒绝：端口已被其他程序占用" +
                    "（关掉串口助手、Arduino IDE 等程序再试）";
        } else if (err == ERROR_FILE_NOT_FOUND) {
            error = "找不到 " + port_name_ + "：请确认设备已插入且驱动已安装";
        } else {
            error = "打开 " + port_name_ + " 失败，Windows 错误码 " +
                    std::to_string(static_cast<unsigned long>(err));
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // 关键点 2：配置串口参数（波特率、数据位、校验、停止位）
    // 这些参数必须和设备端完全一致，否则收到的全是乱码。
    // -----------------------------------------------------------------------
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(handle_, &dcb)) {
        error = "GetCommState 失败";
        Close();
        return false;
    }

    dcb.BaudRate = static_cast<DWORD>(baud_rate_);  // 常用 9600 / 115200
    dcb.ByteSize = 8;                               // 8 位数据位
    dcb.Parity = NOPARITY;                          // 无校验
    dcb.StopBits = ONESTOPBIT;                      // 1 位停止位
    dcb.fBinary = TRUE;                             // 二进制模式，不做字符转换
    // 关闭流控：设备端一般不开硬件流控，开了反而收不到数据
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!::SetCommState(handle_, &dcb)) {
        error = "SetCommState 失败：串口参数被拒绝（检查波特率是否合法）";
        Close();
        return false;
    }

    // -----------------------------------------------------------------------
    // 关键点 3：设置读超时
    //   如果超时全设为 0，ReadFile 会一直阻塞等数据，程序就无法响应 Ctrl+C。
    //   这里设成"最多等 50ms 就返回"，配合非阻塞的 Poll() 语义。
    // -----------------------------------------------------------------------
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 20;          // 两字节之间最多间隔 20ms
    timeouts.ReadTotalTimeoutConstant = 50;     // 每次读最多阻塞 50ms
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    if (!::SetCommTimeouts(handle_, &timeouts)) {
        error = "SetCommTimeouts 失败";
        Close();
        return false;
    }

    // 清空可能残留的旧数据
    ::PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);

    buffer_.clear();
    LOG_INFO("串口已打开：" << Name());
    return true;
#else
    error = "当前平台尚未实现串口支持（本项目目前只实现了 Windows 版本）";
    return false;
#endif
}

void SerialPortSource::Close() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        LOG_INFO("串口已关闭：" << Name());
    }
#endif
    buffer_.clear();
}

ReadResult SerialPortSource::Poll() {
    ReadResult result;

#if defined(_WIN32)
    if (handle_ == INVALID_HANDLE_VALUE) {
        result.error = "串口未打开";
        return result;
    }

    // --- 第 1 步：把硬件缓冲区里的字节全部搬进我们的 buffer_ ---
    char chunk[512];
    DWORD bytes_read = 0;
    if (!::ReadFile(handle_, chunk, sizeof(chunk), &bytes_read, nullptr)) {
        result.error = "ReadFile 失败（设备可能已被拔出）";
        return result;
    }

    if (bytes_read > 0) {
        buffer_.append(chunk, bytes_read);
        LOG_DEBUG("串口收到 " << bytes_read << " 字节，缓冲区现有 "
                              << buffer_.size() << " 字节");
    }

    // --- 第 2 步：从缓冲区里切出一整帧 ---
    const std::size_t newline_pos = buffer_.find('\n');
    if (newline_pos == std::string::npos) {
        // 还没收到换行符 → 这帧不完整，等下次。
        // 但要做长度保护，防止缓冲区无限增长。
        if (buffer_.size() > kMaxBufferBytes) {
            result.error = "缓冲区超过 8KB 仍未收到换行符，已丢弃（可能是波特率不匹配导致乱码）";
            buffer_.clear();
        }
        return result;  // has_data == false，属于正常情况，不是错误
    }

    // 切出这一帧，并从缓冲区里删掉它（包括那个换行符）
    std::string frame = buffer_.substr(0, newline_pos);
    buffer_.erase(0, newline_pos + 1);

    // 处理 Windows 换行是 \r\n 的情况：把结尾的 \r 去掉。
    // 如果不处理，JSON 解析器会把 \r 当成非法字符报错 —— 这是个经典坑。
    if (!frame.empty() && frame.back() == '\r') {
        frame.pop_back();
    }

    result.has_data = true;
    result.frame = std::move(frame);
    return result;
#else
    result.error = "当前平台尚未实现串口支持";
    return result;
#endif
}

std::vector<std::string> SerialPortSource::ListAvailablePorts() {
    std::vector<std::string> ports;

#if defined(_WIN32)
    // 逐个尝试打开 COM1..COM32。
    // 说明：更专业的做法是调 SetupAPI 枚举设备，但那样要引入 setupapi.lib、
    // 代码量翻三倍。这里选用最简单的实现，够用即可 ——
    // 工程上这叫"先让它能工作，再让它更优雅"。
    for (int i = 1; i <= 32; ++i) {
        const std::string name = "COM" + std::to_string(i);
        const std::string path = "\\\\.\\" + name;
        HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ports.push_back(name);
            ::CloseHandle(h);
        }
    }
#endif
    return ports;
}

}  // namespace iotd
