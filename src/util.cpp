#include "util.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace util {

std::string format(const char* fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if ((size_t)n < sizeof(small)) return std::string(small, (size_t)n);
    std::string big((size_t)n + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    big.resize((size_t)n);
    return big;
}

std::string formatTime(double seconds, int decimals) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    const double scale = std::pow(10.0, decimals);
    long long units = (long long)std::floor(seconds * scale + 0.5);
    long long whole = units / (long long)scale;
    long long frac = units % (long long)scale;
    long long h = whole / 3600, m = (whole / 60) % 60, s = whole % 60;
    std::string out = h > 0 ? format("%lld:%02lld:%02lld", h, m, s) : format("%lld:%02lld", m, s);
    if (decimals > 0) out += format(".%0*lld", decimals, frac);
    return out;
}

static size_t lastSep(const std::string& p) { return p.find_last_of("/\\"); }

std::string fileName(const std::string& path) {
    size_t i = lastSep(path);
    return i == std::string::npos ? path : path.substr(i + 1);
}

std::string dirName(const std::string& path) {
    size_t i = lastSep(path);
    if (i == std::string::npos) return std::string();
    if (i == 0) return path.substr(0, 1);
    return path.substr(0, i);
}

std::string stem(const std::string& path) {
    std::string name = fileName(path);
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

std::string extensionLower(const std::string& path) {
    std::string name = fileName(path);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return std::string();
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

bool pathExists(const std::string& path) { return !path.empty() && SDL_GetPathInfo(path.c_str(), nullptr); }

bool removeFile(const std::string& path) { return SDL_RemovePath(path.c_str()); }

bool isSameFile(const std::string& a, const std::string& b) {
    std::error_code ec;
    auto pa = std::filesystem::u8path(a), pb = std::filesystem::u8path(b);
    if (!std::filesystem::exists(pa, ec) || !std::filesystem::exists(pb, ec)) return false;
    return std::filesystem::equivalent(pa, pb, ec);
}

std::string executableDir() {
    const char* base = SDL_GetBasePath();
    return base ? std::string(base) : std::string();
}

}  // namespace util

void MainQueue::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(m_);
        q_.push_back(std::move(fn));
    }
    wakeMainLoop();
}

void MainQueue::run() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_);
        tasks.swap(q_);
    }
    for (auto& t : tasks) t();
}

Worker::Worker() { thread_ = std::thread(&Worker::loop, this); }

Worker::~Worker() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
        q_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Worker::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(m_);
        q_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void Worker::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (stop_) return;
            job = std::move(q_.front());
            q_.pop_front();
        }
        job();
    }
}
