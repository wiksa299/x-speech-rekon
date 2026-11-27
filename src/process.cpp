#include "core.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace xsr {

fs::path executable_path(const char* argv0) {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (length && length < path.size()) return fs::path(path.substr(0, length));
#endif
    return fs::absolute(fs::u8path(argv0));
}

#ifdef _WIN32
using Pipe = HANDLE;
constexpr Pipe no_pipe = nullptr;
static void close_pipe(Pipe& pipe) {
    if (pipe) CloseHandle(std::exchange(pipe, no_pipe));
}
#else
using Pipe = int;
constexpr Pipe no_pipe = -1;
static void close_pipe(Pipe& pipe) {
    if (pipe != no_pipe) close(std::exchange(pipe, no_pipe));
}
#endif

struct Process::Impl {
    explicit Impl(fs::path executable) : path(std::move(executable)) {}
    fs::path path;
    Pipe from_engine = no_pipe;
    Pipe to_engine = no_pipe;
#ifdef _WIN32
    HANDLE child = nullptr;
    HANDLE job = nullptr;
#else
    pid_t child = -1;
#endif
    std::thread reader;
    std::thread writer;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::string> outbound;
    std::vector<Json> inbound;
    std::atomic<bool> dead{true};

    bool spawn();
    void terminate();
    void read_messages();
    void write_messages();
    void fail() {
        {
            std::lock_guard lock(mutex);
            dead = true;
        }
        available.notify_all();
    }
};

#ifdef _WIN32
bool Process::Impl::spawn() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE input = nullptr, output = nullptr, error = nullptr;
    bool success = false;
    if (CreatePipe(&input, &to_engine, &security, 0) &&
        CreatePipe(&from_engine, &output, &security, 0) &&
        SetHandleInformation(from_engine, HANDLE_FLAG_INHERIT, 0) &&
        SetHandleInformation(to_engine, HANDLE_FLAG_INHERIT, 0)) {
        error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (error != INVALID_HANDLE_VALUE) {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input;
            startup.StartupInfo.hStdOutput = output;
            startup.StartupInfo.hStdError = error;
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<unsigned char> storage(size);
            startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
                HANDLE handles[] = {input, output, error};
                if (UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) {
                    job = CreateJobObjectW(nullptr, nullptr);
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    PROCESS_INFORMATION process{};
                    auto executable = path.wstring();
                    auto command = L"\"" + executable + L"\"";
                    if (job && SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                                       &limits, sizeof(limits)) &&
                        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &process)) {
                        child = process.hProcess;
                        success = AssignProcessToJobObject(job, child) &&
                                  ResumeThread(process.hThread) != DWORD(-1);
                        if (!success) TerminateProcess(child, 1);
                        CloseHandle(process.hThread);
                    }
                }
                DeleteProcThreadAttributeList(startup.lpAttributeList);
            }
            close_pipe(error);
        }
    }
    close_pipe(input);
    close_pipe(output);
    return success;
}

void Process::Impl::terminate() {
    if (child) {
        if (WaitForSingleObject(child, 200) == WAIT_TIMEOUT) {
            TerminateProcess(child, 1);
            WaitForSingleObject(child, 500);
        }
        close_pipe(child);
    }
    close_pipe(job);
}

#else
bool Process::Impl::spawn() {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    bool success = false;
    if (pipe2(input, O_CLOEXEC) == 0 && pipe2(output, O_CLOEXEC) == 0) {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) == 0) {
            auto executable = path.string();
            char* args[] = {executable.data(), nullptr};
            if (posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO) == 0 &&
                posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0 &&
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0) {
                pid_t pid = -1;
                if (posix_spawn(&pid, executable.c_str(), &actions, nullptr, args, environ) == 0) {
                    child = pid;
                    to_engine = std::exchange(input[1], no_pipe);
                    from_engine = std::exchange(output[0], no_pipe);
                    success = true;
                }
            }
            posix_spawn_file_actions_destroy(&actions);
        }
    }
    for (int& fd : input) close_pipe(fd);
    for (int& fd : output) close_pipe(fd);
    return success;
}

void Process::Impl::terminate() {
    if (child < 0) return;
    auto result = waitpid(child, nullptr, WNOHANG);
    for (int i = 0; result == 0 && i < 10; ++i) {
        std::this_thread::sleep_for(10ms);
        result = waitpid(child, nullptr, WNOHANG);
    }
    if (result == 0 || (result < 0 && errno == EINTR)) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    child = -1;
}
#endif

void Process::Impl::read_messages() {
    std::string pending;
    char buffer[4096];
    while (!dead) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(from_engine, buffer, sizeof(buffer), &count, nullptr) || !count) break;
#else
        const auto count = read(from_engine, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
#endif
        bool invalid = false;
        for (size_t offset = 0; offset < size_t(count); ++offset) {
            if (buffer[offset] != '\n') {
                pending.push_back(buffer[offset]);
                if (pending.size() > max_frame) { invalid = true; break; }
                continue;
            }
            auto message = Json::parse(pending, nullptr, false);
            pending.clear();
            if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) { invalid = true; break; }
            std::lock_guard lock(mutex);
            if (inbound.size() >= 128) { invalid = true; break; }
            inbound.push_back(std::move(message));
        }
        if (invalid) break;
    }
    fail();
}

void Process::Impl::write_messages() {
#ifndef _WIN32
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
    while (!dead) {
        std::string next;
        {
            std::unique_lock lock(mutex);
            available.wait(lock, [&] { return dead || !outbound.empty(); });
            if (dead) break;
            next = std::move(outbound.front());
            outbound.pop_front();
        }
        size_t offset = 0;
        while (offset < next.size()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(to_engine, next.data() + offset, DWORD(next.size() - offset), &count, nullptr) ||
                !count) { fail(); break; }
#else
            const auto count = write(to_engine, next.data() + offset, next.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { fail(); break; }
#endif
            offset += size_t(count);
        }
    }
}

Process::Process(fs::path executable) : impl_(std::make_unique<Impl>(fs::absolute(executable))) {}
Process::~Process() { stop(); }

bool Process::start() {
    stop();
    auto& state = *impl_;
    if (!state.spawn()) {
        stop();
        return false;
    }
    {
        std::lock_guard lock(state.mutex);
        state.dead = false;
    }
    try {
        state.reader = std::thread([&state] { state.read_messages(); });
        state.writer = std::thread([&state] { state.write_messages(); });
    } catch (const std::system_error&) {
        stop();
        return false;
    }
    return true;
}

bool Process::send(const Json& message) {
    auto line = wire_frame(message);
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    const auto type = message.value("type", "");
    const bool control = type == "end" || type == "cancel" || type == "shutdown" || type == "cancel_download" ||
        type == "mic_stop" || type == "mic_clear";
    if (state.dead || line.empty() || state.outbound.size() >= (control ? 68u : 64u)) return false;
    state.outbound.push_back(std::move(line));
    state.available.notify_one();
    return true;
}

std::vector<Json> Process::receive() {
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    std::vector<Json> messages;
    messages.swap(state.inbound);
    return messages;
}

bool Process::failed() const { return impl_->dead; }

void Process::stop() {
    auto& state = *impl_;
    if (!state.dead && state.writer.joinable()) {
        send({{"type", "shutdown"}});
#ifdef _WIN32
        if (state.child) WaitForSingleObject(state.child, 75);
#else
        std::this_thread::sleep_for(25ms);
#endif
    }
    state.fail();
    state.terminate();
    if (state.writer.joinable()) state.writer.join();
    if (state.reader.joinable()) state.reader.join();
    close_pipe(state.to_engine);
    close_pipe(state.from_engine);
    std::lock_guard lock(state.mutex);
    state.outbound.clear();
    state.inbound.clear();
}

} // namespace xsr
