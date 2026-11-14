#include "process.h"

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
// Spawning is serialized so that pipe handles created for one child can never
// be inherited by another child started concurrently from a different thread.
std::mutex g_spawnMutex;
}  // namespace

#ifdef _WIN32

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string errorString(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::string s = buf ? narrow(buf) : std::string("error ") + std::to_string(code);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Quotes one argument following the MSVC runtime parsing rules.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t i = 0;
    for (;;) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(backslashes, L'\\');
            cmd += arg[i];
        }
        ++i;
    }
    cmd += L'"';
}
}  // namespace

bool Process::start(const std::vector<std::string>& args, Stderr stderrMode, std::string* error) {
    if (args.empty() || process_) return false;

    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd += L' ';
        appendQuoted(cmd, widen(args[i]));
    }

    std::lock_guard<std::mutex> lock(g_spawnMutex);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        if (error) *error = errorString(GetLastError());
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = (stderrMode == Stderr::Merge) ? wr : nul;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();

    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (error) *error = args[0] + ": " + errorString(err);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pipe_ = rd;
    waited_ = false;
    exitCode_ = -1;
    return true;
}

int Process::read(char* buf, int size, int timeoutMs) {
    if (!pipe_ || size <= 0) return -1;
    HANDLE h = (HANDLE)pipe_;
    if (timeoutMs < 0) {
        DWORD got = 0;
        if (!ReadFile(h, buf, (DWORD)size, &got, nullptr) || got == 0) return -1;
        return (int)got;
    }
    ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return -1;  // broken pipe == EOF
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(h, buf, std::min<DWORD>(avail, (DWORD)size), &got, nullptr) || got == 0) return -1;
            return (int)got;
        }
        if (GetTickCount64() - start >= (ULONGLONG)timeoutMs) return 0;
        Sleep(5);
    }
}

void Process::terminate() {
    if (process_ && !waited_) TerminateProcess((HANDLE)process_, 1);
}

int Process::wait() {
    if (waited_) return exitCode_;
    if (!process_) return -1;
    WaitForSingleObject((HANDLE)process_, INFINITE);
    DWORD code = 0;
    exitCode_ = GetExitCodeProcess((HANDLE)process_, &code) ? (int)code : -1;
    CloseHandle((HANDLE)process_);
    process_ = nullptr;
    waited_ = true;
    return exitCode_;
}

void Process::closePipe() {
    if (pipe_) {
        CloseHandle((HANDLE)pipe_);
        pipe_ = nullptr;
    }
}

Process::~Process() {
    if (process_ && !waited_) {
        terminate();
        wait();
    }
    closePipe();
}

#else  // POSIX

bool Process::start(const std::vector<std::string>& args, Stderr stderrMode, std::string* error) {
    if (args.empty() || pid_ > 0) return false;

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::lock_guard<std::mutex> lock(g_spawnMutex);

    int fds[2];
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) != 0) {
#else
    if (pipe(fds) != 0 || fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
#endif
        if (error) *error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    if (stderrMode == Stderr::Merge)
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

    // Children start with a clean signal state so that terminate() always works.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(fds[1]);

    if (rc != 0) {
        ::close(fds[0]);
        if (error) *error = args[0] + ": " + std::strerror(rc);
        return false;
    }
    pid_ = pid;
    fd_ = fds[0];
    waited_ = false;
    exitCode_ = -1;
    return true;
}

int Process::read(char* buf, int size, int timeoutMs) {
    if (fd_ < 0 || size <= 0) return -1;
    for (;;) {
        pollfd p{};
        p.fd = fd_;
        p.events = POLLIN;
        int r = ::poll(&p, 1, timeoutMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0;
        ssize_t n = ::read(fd_, buf, (size_t)size);
        if (n > 0) return (int)n;
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return -1;  // EOF or error
    }
}

void Process::terminate() {
    // The pid cannot be recycled before wait() reaps it, so this is always safe.
    if (pid_ > 0 && !waited_) ::kill(pid_, SIGTERM);
}

int Process::wait() {
    if (waited_) return exitCode_;
    if (pid_ <= 0) return -1;
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(pid_, &status, 0);
    } while (r < 0 && errno == EINTR);
    waited_ = true;
    exitCode_ = (r == pid_ && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return exitCode_;
}

void Process::closePipe() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Process::~Process() {
    if (pid_ > 0 && !waited_) {
        terminate();
        wait();
    }
    closePipe();
}

#endif

int runCapture(const std::vector<std::string>& args, Process::Stderr stderrMode, std::string& output,
               size_t maxBytes) {
    output.clear();
    Process p;
    std::string err;
    if (!p.start(args, stderrMode, &err)) {
        output = err;
        return -1;
    }
    char buf[16384];
    for (;;) {
        int n = p.read(buf, (int)sizeof(buf), -1);
        if (n < 0) break;
        if (output.size() + (size_t)n > maxBytes) {
            p.terminate();
            break;
        }
        output.append(buf, (size_t)n);
    }
    return p.wait();
}
