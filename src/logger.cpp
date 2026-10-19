// ===========================================================================
// src/logger.cpp
// ===========================================================================

#include "iotd/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <sstream>

namespace iotd {

Logger& Logger::Instance() {
    // C++11 起，函数内 static 变量的初始化是"线程安全"的，
    // 并且只会在第一次调用时初始化一次，这是实现单例最简洁的写法
    static Logger instance;
    return instance;
}

const char* Logger::LevelName(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

std::string Logger::NowString() {
    using namespace std::chrono;

    // 取当前时间点，转成系统时间
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);

    // 算毫秒部分：总毫秒数对 1000 取余
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t); // Windows 的安全版本
#else
    localtime_r(&t, &tm_buf); // Linux/macOS 版本
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms));
    return buf;
}

bool Logger::OpenFile(const std::string& path, bool append) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(path, append ? (std::ios::out | std::ios::app) : std::ios::out);
    return file_.is_open();
}

void Logger::Log(Level level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(min_level_)) {
        return; // 级别不够，直接丢弃
    }

    // 【加锁】整个输出过程必须原子完成
    // 把 NowString() 也放进锁里，是为了保证同一行的"时间戳"和"内容"
    // 确实来自同一次调用，不会出现 A 的时间戳配上 B 的内容
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string line = NowString() + " [" + LevelName(level) + "] " + message;

    // 屏幕输出，用 std::cerr 输出，保证它不会被缓冲区吞掉
    std::cerr << line << '\n';

    // 文件输出，每写一行立刻 flush ——
    // 因为程序崩溃时，缓冲区里没落盘的内容会丢失，
    // 而崩溃前最后几行日志往往正是你最需要看的
    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
}

void Logger::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }
}

} // namespace iotd
