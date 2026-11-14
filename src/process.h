// Minimal cross-platform child process (Linux/POSIX + Windows).
// - No shell involved: arguments are passed as-is (safe with spaces/quotes/unicode).
// - stdout (and optionally stderr) is read through a pipe.
// - On Windows the child never opens a console window.
// A Process object must be used from a single thread.
#pragma once

#include <string>
#include <vector>

class Process {
public:
    enum class Stderr { Merge, Discard };

    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // args[0] is the program (absolute path or a name searched in PATH).
    bool start(const std::vector<std::string>& args, Stderr stderrMode, std::string* error = nullptr);

    // Reads available output.
    // Returns >0 bytes read, 0 if nothing arrived within timeoutMs, -1 on EOF/error.
    // timeoutMs < 0 waits indefinitely.
    int read(char* buf, int size, int timeoutMs);

    void terminate();  // ask the child to stop (SIGTERM / TerminateProcess)
    int wait();        // blocks until exit; returns exit code (-1 if unknown)

private:
    void closePipe();
#ifdef _WIN32
    void* process_ = nullptr;  // HANDLE
    void* pipe_ = nullptr;     // HANDLE (read end)
#else
    int pid_ = -1;
    int fd_ = -1;
#endif
    bool waited_ = false;
    int exitCode_ = -1;
};

// Runs a program to completion and captures its output. Returns the exit code,
// or -1 if it could not be started (error text in `output`).
int runCapture(const std::vector<std::string>& args, Process::Stderr stderrMode, std::string& output,
               size_t maxBytes = 64 * 1024 * 1024);
