#include "core.hpp"

#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPanelGraphics.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <dlfcn.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace xvr;

namespace {
struct Plugin {
    void* library = nullptr;
    int (*start)(char*, char*, char*) = nullptr;
    int (*enable)() = nullptr;
    void (*disable)() = nullptr;
    void (*stop)() = nullptr;
    void (*receive)(XPLMPluginID, int, void*) = nullptr;
    std::string path, title, signature;
    bool enabled = false;
    XPLMCreateWindow_t window{};
    XPLMMenuHandler_f menu = nullptr;
    void* menu_data = nullptr;
    XPLMFlightLoop_f flight = nullptr;
    void* flight_data = nullptr;
    XPLMCommandCallback_f command = nullptr;
    void* command_data = nullptr;
};
Plugin plugins[3];
int current = 0, drawn = 0;
std::string prefs;
auto started = Clock::now();
XPLMLanguageCode language = xplm_Language_French;

struct AsPlugin {
    int previous;
    explicit AsPlugin(int id) : previous(current) { current = id; }
    ~AsPlugin() { current = previous; }
};

void require(bool condition, const char* hint) {
    if (!condition) throw std::runtime_error(hint);
}

template<class T>
void bind(Plugin& plug, T& callback, const char* name) {
    callback = reinterpret_cast<T>(dlsym(plug.library, name));
    require(callback != nullptr, name);
}

void load(int id, const char* binary, const fs::path& apparent) {
    auto& plug = plugins[id];
    plug.path = apparent.u8string();
    plug.library = dlopen(binary, RTLD_NOW | RTLD_LOCAL);
    if (!plug.library) throw std::runtime_error(dlerror());
    bind(plug, plug.start, "XPluginStart");
    bind(plug, plug.enable, "XPluginEnable");
    bind(plug, plug.disable, "XPluginDisable");
    bind(plug, plug.stop, "XPluginStop");
    bind(plug, plug.receive, "XPluginReceiveMessage");
    AsPlugin as(id);
    char title[256]{}, signature[256]{}, description[256]{};
    require(plug.start(title, signature, description) == 1, "start");
    plug.title = title;
    plug.signature = signature;
    plug.enabled = plug.enable() == 1;
    require(plug.enabled, "enable");
}

void draw(int id) {
    AsPlugin as(id);
    auto& plug = plugins[id];
    plug.menu(plug.menu_data, nullptr);
    require(plug.window.visible, "window visible after menu item");
    const int before = drawn;
    require(plug.window.contentType == xplm_WindowContentTypePanelGraphics, "panel graphics");
    plug.window.drawWindowFunc(reinterpret_cast<XPLMWindowID>(&plug.window), plug.window.refcon);
    require(drawn > before, "Dear ImGui emitted draw calls");
}

void pump(int id) {
    AsPlugin as(id);
    auto& plug = plugins[id];
    plug.flight(0.01f, 0.01f, 0, plug.flight_data);
}
} // namespace

XPLMPluginID XPLMGetMyID() { return current; }
void XPLMGetSystemPath(char* path) { std::strcpy(path, fs::path(prefs).parent_path().c_str()); }
int XPLMIsPluginEnabled(XPLMPluginID id) { return id > 0 && id < 3 && plugins[id].enabled; }
XPLMPluginID XPLMFindPluginBySignature(const char* signature) {
    for (int i = 1; i < 3; ++i) if (plugins[i].signature == signature) return i;
    return XPLM_NO_PLUGIN_ID;
}
void XPLMGetPluginInfo(XPLMPluginID id, char* name, char* path, char* sig, char* description) {
    const auto& plug = plugins[id];
    if (name) std::strcpy(name, plug.title.c_str());
    if (path) std::strcpy(path, plug.path.c_str());
    if (sig) std::strcpy(sig, plug.signature.c_str());
    if (description) *description = '\0';
}
void XPLMEnableFeature(const char*, int) {}
void XPLMSendMessageToPlugin(XPLMPluginID id, int message, void* parameter) {
    if (!XPLMIsPluginEnabled(id)) return;
    const int sender = current;
    AsPlugin as(id);
    plugins[id].receive(sender, message, parameter);
}
void XPLMGetPrefsPath(char* value) { std::strcpy(value, prefs.c_str()); }
void XPLMDebugString(const char*) {}
XPLMLanguageCode XPLMGetLanguage() { return language; }
float XPLMGetElapsedTime() { return std::chrono::duration<float>(Clock::now() - started).count(); }

XPLMMenuID XPLMFindPluginsMenu() { return reinterpret_cast<XPLMMenuID>(plugins); }
int XPLMAppendMenuItem(XPLMMenuID, const char*, void*, int) { return 0; }
void XPLMRemoveMenuItem(XPLMMenuID, int) {}
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f handler, void* owner) {
    plugins[current].menu = handler;
    plugins[current].menu_data = owner;
    return reinterpret_cast<XPLMMenuID>(&plugins[current]);
}
void XPLMDestroyMenu(XPLMMenuID) {}
void XPLMRegisterFlightLoopCallback(XPLMFlightLoop_f callback, float, void* owner) {
    plugins[current].flight = callback;
    plugins[current].flight_data = owner;
}
void XPLMUnregisterFlightLoopCallback(XPLMFlightLoop_f, void*) { plugins[current].flight = nullptr; }
XPLMCommandRef XPLMCreateCommand(const char*, const char*) { return reinterpret_cast<XPLMCommandRef>(&plugins[current]); }
void XPLMRegisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f callback, int, void* owner) {
    plugins[current].command = callback; plugins[current].command_data = owner;
}
void XPLMUnregisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f, int, void*) { plugins[current].command = nullptr; }

void XPLMGetScreenBoundsGlobal(int* left, int* top, int* right, int* bottom) {
    *left = 0; *top = 1024; *right = 1280; *bottom = 0;
}
void XPLMGetMouseLocationGlobal(int* x, int* y) { *x = 0; *y = 0; }
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* params) {
    auto& window = plugins[current].window;
    window = *params;
    return reinterpret_cast<XPLMWindowID>(&window);
}
void XPLMDestroyWindow(XPLMWindowID) {}
int XPLMGetWindowIsVisible(XPLMWindowID id) { return reinterpret_cast<XPLMCreateWindow_t*>(id)->visible; }
void XPLMSetWindowIsVisible(XPLMWindowID id, int visible) { reinterpret_cast<XPLMCreateWindow_t*>(id)->visible = visible; }
void XPLMBringWindowToFront(XPLMWindowID) {}
void XPLMSetWindowTitle(XPLMWindowID, const char*) {}
void XPLMGetWindowGeometry(XPLMWindowID id, int* left, int* top, int* right, int* bottom) {
    const auto& w = *reinterpret_cast<XPLMCreateWindow_t*>(id);
    *left = w.left; *top = w.top; *right = w.right; *bottom = w.bottom;
}
void* XPLMCreateTexture(const unsigned char* rgba, int width, int height) {
    require(rgba && width > 0 && height > 0, "font atlas");
    return &plugins[current];
}
void XPLMDestroyTexture(void*) {}
void XPLMTransformPush() {}
void XPLMTransformPop() {}
void XPLMTransformTranslate(float, float) {}
void XPLMTransformScale(float, float) {}
void XPLMDrawCalls(const XPLMMesh_t* mesh, int count, const XPLMDrawCall_t* commands) {
    require(mesh && mesh->vertex_count > 0 && mesh->index_count > 0, "draw mesh");
    for (int i = 0; i < count; ++i) {
        require(commands[i].tex_ref != nullptr && commands[i].element_count % 3 == 0, "draw call");
        ++drawn;
    }
}

int main(int argc, char** argv) {
    try {
        require(argc == 5, "provider diagnostic engine catalog");
        const auto root = fs::temp_directory_path() / ("xvr-native-" + std::to_string(
            Clock::now().time_since_epoch().count()));
        struct Fixture {
            fs::path root;
            ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
        } fixture{root};
        fs::create_directories(root / "bin");
        fs::create_symlink(fs::absolute(argv[3]), root / "bin/xvr-engine");
        fs::copy_file(argv[4], root / "catalog.json");
        fs::copy_file(fs::path(argv[4]).parent_path() / "NotoSans-Regular.ttf", root / "NotoSans-Regular.ttf");
        prefs = (root / "X-Plane.prf").u8string();

        load(1, argv[1], root / "64/lin.xpl");
        load(2, argv[2], root / "diagnostic/64/lin.xpl");
        pump(1);
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(10ms);
            pump(1);
            pump(2);
        }
        draw(1);
        language = xplm_Language_English;
        draw(1);
        language = xplm_Language_Spanish;
        auto& provider_window = plugins[1].window;
        provider_window.right = provider_window.left + 560;
        provider_window.bottom = provider_window.top - 460;
        draw(1);
        draw(2);
        {
            AsPlugin as(2);
            XvrRequest request{};
            request.version = XVR_ABI_VERSION;
            request.struct_size = sizeof(request);
            request.operation = XVR_PTT_BEGIN;
            request.status = XVR_SERVICE_UNAVAILABLE;
            XPLMSendMessageToPlugin(1, XVR_MESSAGE_REQUEST, &request);
            require(request.status == XVR_MODEL_UNAVAILABLE, "diagnostic subscribed to provider");
            require(plugins[2].command != nullptr, "assignable command");
            plugins[2].command(nullptr, xplm_CommandBegin, plugins[2].command_data);
            plugins[2].command(nullptr, xplm_CommandEnd, plugins[2].command_data);
        }
        for (int id : {1, 2}) {
            AsPlugin as(id);
            auto& plug = plugins[id];
            plug.disable();
            plug.enabled = false;
            plug.stop();
            require(plug.flight == nullptr, "flight loop unregistered");
            dlclose(plug.library);
        }
        std::cout << "Native module lifecycle, inter-plugin command and ImGui drawing passed in the SDK test host\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
