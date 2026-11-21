#include "desktop.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#include <shobjidl.h>
#else
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace xsr {

#ifdef _WIN32
FileSelection browse_path(bool folder, const std::atomic<bool>&) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return {{}, true};
    struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } uninitialize;
    IFileDialog* raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&raw)))) return {{}, true};
    const auto release = [](auto* value) { if (value) value->Release(); };
    std::unique_ptr<IFileDialog, decltype(release)> dialog(raw, release);
    const auto options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST |
        (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
    if (FAILED(dialog->SetOptions(options))) return {{}, true};
    const COMDLG_FILTERSPEC filter{L"WAV", L"*.wav"};
    if (!folder && FAILED(dialog->SetFileTypes(1, &filter))) return {{}, true};
    const auto shown = dialog->Show(nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(shown)) return {{}, true};
    IShellItem* selected = nullptr;
    if (FAILED(dialog->GetResult(&selected))) return {{}, true};
    std::unique_ptr<IShellItem, decltype(release)> item(selected, release);
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {{}, true};
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> text(path, CoTaskMemFree);
    return {std::filesystem::path(text.get()).u8string()};
}

bool copy_diagnostic_text(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (!length) return false;
    const auto handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(length) * sizeof(wchar_t));
    if (!handle) return false;
    auto* buffer = static_cast<wchar_t*>(GlobalLock(handle));
    const bool converted = buffer &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, buffer, length) != 0;
    if (buffer) GlobalUnlock(handle);

    bool copied = false;
    if (converted) {
        const auto owner = CreateWindowExW(0, L"Static", nullptr, 0, 0, 0, 0, 0,
                                          HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (owner && OpenClipboard(owner)) {
            copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, handle);
            CloseClipboard();
        }
        if (owner) DestroyWindow(owner);
    }
    if (!copied) GlobalFree(handle);
    return copied;
}
#else

namespace {
constexpr int command_unavailable = -1;
constexpr int command_failed = -2;

int run_tool(std::vector<const char*> args, FILE* input, FILE* output,
             const std::atomic<bool>* stopping, std::chrono::seconds timeout = {}) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return command_failed;
    const auto redirect = [&](FILE* file, int descriptor, int flags) {
        return file ? posix_spawn_file_actions_adddup2(&actions, fileno(file), descriptor) :
                      posix_spawn_file_actions_addopen(&actions, descriptor, "/dev/null", flags, 0);
    };
    pid_t child = -1;
    int launched = 0;
    if (redirect(input, STDIN_FILENO, O_RDONLY) || redirect(output, STDOUT_FILENO, O_WRONLY) ||
        redirect(nullptr, STDERR_FILENO, O_WRONLY)) {
        launched = EIO;
    } else {
        std::vector<char*> argv;
        for (auto* arg : args) argv.push_back(const_cast<char*>(arg));
        argv.push_back(nullptr);
        launched = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (launched) return launched == ENOENT ? command_unavailable : command_failed;

    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : command_failed;
        if (result < 0 && errno != EINTR) return command_failed;
        if ((stopping && *stopping) || (timeout.count() && std::chrono::steady_clock::now() >= deadline)) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return command_failed;
        }
        std::this_thread::sleep_for(15ms);
    }
}

struct CloseFile {
    void operator()(FILE* file) const { std::fclose(file); }
};
using TemporaryFile = std::unique_ptr<FILE, CloseFile>;
}

FileSelection browse_path(bool folder, const std::atomic<bool>& stopping) {
    if (folder || (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))) return {{}, true};
    TemporaryFile output(std::tmpfile());
    if (!output) return {{}, true};
    int result = run_tool({"zenity", "--file-selection", "--file-filter=WAV | *.wav *.WAV"},
                          nullptr, output.get(), &stopping);
    if (result == command_unavailable) {
        result = run_tool({"kdialog", "--getopenfilename", ".", "*.wav *.WAV|WAV"},
                          nullptr, output.get(), &stopping);
    }
    if (result == 1) return {};
    if (result != 0 || std::fseek(output.get(), 0, SEEK_SET) != 0) return {{}, true};
    std::array<char, 16384> bytes{};
    const auto count = std::fread(bytes.data(), 1, bytes.size(), output.get());
    if (!count || count == bytes.size() || std::ferror(output.get())) return {{}, true};
    std::string path(bytes.data(), count);
    if (path.back() == '\n') path.pop_back();
    return {std::move(path)};
}

bool copy_diagnostic_text(const std::string& text) {
    TemporaryFile input(std::tmpfile());
    if (!input || std::fwrite(text.data(), 1, text.size(), input.get()) != text.size()) return false;
    const auto copy = [&](std::vector<const char*> command) {
        return std::fseek(input.get(), 0, SEEK_SET) == 0 &&
               run_tool(std::move(command), input.get(), nullptr, nullptr, std::chrono::seconds(3)) == 0;
    };
    return (std::getenv("WAYLAND_DISPLAY") && copy({"wl-copy", "--type", "text/plain;charset=utf-8"})) ||
           (std::getenv("DISPLAY") && copy({"xclip", "-selection", "clipboard", "-in", "-silent"}));
}
#endif

} // namespace xsr
