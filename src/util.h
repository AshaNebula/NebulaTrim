#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace util {

std::string format(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string formatTime(double seconds, int decimals = 2);  // "1:02.35" / "1:02:03.35"

std::string fileName(const std::string& path);
std::string dirName(const std::string& path);
std::string stem(const std::string& path);
std::string extensionLower(const std::string& path);  // without the dot
std::string joinPath(const std::string& dir, const std::string& name);

bool pathExists(const std::string& path);
bool removeFile(const std::string& path);
bool isSameFile(const std::string& a, const std::string& b);
std::string executableDir();  // with trailing separator

}  // namespace util

// Wakes up the main loop from any thread (implemented in main.cpp).
void wakeMainLoop();

// Closures executed later on the main thread (thread-safe post).
class MainQueue {
public:
    void post(std::function<void()> fn);
    void run();

private:
    std::mutex m_;
    std::vector<std::function<void()>> q_;
};

// One background thread that runs jobs in order.
class Worker {
public:
    Worker();
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void submit(std::function<void()> job);

private:
    void loop();

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    std::thread thread_;
};
