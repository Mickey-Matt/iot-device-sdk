// ===========================================================================
// iotd/logger.h
//
// 一个极简日志器：同时输出到屏幕和文件。
//
// 【为什么这个文件很重要】
// 你要投的是"技术支持"方向的岗位。这个岗位每天做的事就是：
// 从日志里看出问题出在哪一段链路上。
// 所以"会打日志"不是可选项，是这个岗位的核心技能。
//
// 一个合格的日志必须包含：
//   1. 时间戳      —— 用于和 broker 日志、抓包时间对齐
//   2. 日志级别    —— 用于过滤（平时看 INFO，排查看 DEBUG）
//   3. 出错的原始内容 —— 排查时最需要的东西
// ===========================================================================

#pragma once

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace iotd {

class Logger {
public:
    enum class Level { Debug, Info, Warn, Error };

    // 单例模式：全程序只有一个 Logger，任何地方都能通过 Logger::Instance() 拿到它。
    //
    // 为什么要单例？因为日志是"全局横切关注点"——
    // 几乎每个函数都想记日志，如果靠参数一层层传 Logger，代码会被污染得很难看。
    // 单例的代价是"全局状态"（难测试），所以真实项目里通常用依赖注入。
    // 这里为了让你先跑起来，用最简单的做法，代码注释里写明这是折中。
    static Logger& Instance();

    // 打开文件日志。append=true 表示追加而不是覆盖（默认行为，排查问题时需要保留历史）
    bool OpenFile(const std::string& path, bool append = true);

    // 设置最低输出级别：低于这个级别的不打印
    void SetLevel(Level min_level) { min_level_ = min_level; }

    void Log(Level level, const std::string& message);

    // 关闭文件
    void Close();

private:
    Logger() = default;             // 构造函数私有 → 外部无法 new Logger
    Logger(const Logger&) = delete; // 禁止拷贝
    Logger& operator=(const Logger&) = delete;

    static const char* LevelName(Level level);
    static std::string NowString(); // 生成 "2026-09-30 14:05:12.123" 形式的时间戳

    Level min_level_ = Level::Info;
    std::ofstream file_;

    // -----------------------------------------------------------------------
    // 线程安全保护
    //
    // 【这是踩坑之后补上的】
    // 引入 MQTT 后台线程后，日志里出现了重复行 —— 因为 std::ofstream 和
    // std::cerr 都不是线程安全的：两个线程同时写同一个流，
    // 内部缓冲区会被交错破坏，症状就是"某行日志打印两次"或内容错乱。
    //
    // 修法：给输出加锁，保证同一时刻只有一个线程在写。
    // mutable 是必需的：Log() 在语义上是"观察"操作，但加锁要修改 mutex 状态。
    //
    // 这也说明第一次那个坑为什么危险：
    // 一旦程序里有了第二个线程，"打日志"这种看起来最无害的事也会出问题。
    // -----------------------------------------------------------------------
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// 便捷宏：让你写日志时不用敲 Logger::Instance().Log(Logger::Level::Info, ...)
//
// 宏用 #x 把表达式变成字符串，
//   LOG_INFO("temp=" << t)  →  输出类似 "temp=25.3"
// 这样就像用 std::cout 一样自然。
// ---------------------------------------------------------------------------
#define IOTD_LOG(level, expr)                                                                      \
    do {                                                                                           \
        std::ostringstream _oss;                                                                   \
        _oss << expr;                                                                              \
        ::iotd::Logger::Instance().Log(level, _oss.str());                                         \
    } while (false)
// 外面套 do { } while(false) 是宏的经典防御写法：
// 保证它在 if/else 里当成一条语句，不会因为缺分号而出错。

#define LOG_DEBUG(expr) IOTD_LOG(::iotd::Logger::Level::Debug, expr)
#define LOG_INFO(expr) IOTD_LOG(::iotd::Logger::Level::Info, expr)
#define LOG_WARN(expr) IOTD_LOG(::iotd::Logger::Level::Warn, expr)
#define LOG_ERROR(expr) IOTD_LOG(::iotd::Logger::Level::Error, expr)

} // namespace iotd
