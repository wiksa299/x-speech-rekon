#include "core.hpp"
#include "window.hpp"
#include "localization.hpp"

#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPanelGraphics.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace xsr;

int locale_test_engine();

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
    XPLMCreateWindow_t window{}, overlay{};
    XPLMMenuHandler_f menu = nullptr;
    void* menu_data = nullptr;
    XPLMFlightLoop_f flight = nullptr;
    void* flight_data = nullptr;
    XPLMCommandCallback_f command = nullptr;
    void* command_data = nullptr;
};
Plugin plugins[3];
struct RegisteredDataRef {
    XPLMGetDatai_f read;
    void* context;
};
std::map<std::string, RegisteredDataRef> datarefs;
std::string delivered_text, delivered_language;
uint64_t captured_session = 0, delivered_session = 0;
int current = 0, drawn = 0;
int vertices = 0, mouse_x = 0, mouse_y = 0;
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

void click(int x, int y) {
    AsPlugin as(1);
    auto& window = plugins[1].window;
    const auto id = reinterpret_cast<XPLMWindowID>(&window);
    mouse_x = window.left + x;
    mouse_y = window.top - y;
    draw(1);
    window.handleMouseClickFunc(id, mouse_x, mouse_y, xplm_MouseDown, window.refcon);
    draw(1);
    window.handleMouseClickFunc(id, mouse_x, mouse_y, xplm_MouseUp, window.refcon);
    draw(1);
    mouse_x = mouse_y = 0;
}

void pump(int id) {
    AsPlugin as(id);
    auto& plug = plugins[id];
    plug.flight(0.01f, 0.01f, 0, plug.flight_data);
}

int read_dataref(const char* name) {
    const auto& ref = datarefs.at(name);
    return ref.read(ref.context);
}

void wait_for_engine(bool pump_diagnostic = false) {
    const auto limit = Clock::now() + 3s;
    do {
        pump(1);
        if (pump_diagnostic) pump(2);
        if (read_dataref(XSR_DATAREF_SERVICE_READY)) return;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < limit);
    require(false, "engine became ready");
}

XsrRequest ptt_from_diagnostic(uint32_t operation = XSR_PTT_BEGIN, uint64_t session = 0) {
    AsPlugin as(2);
    XsrRequest request{};
    request.version = XSR_ABI_VERSION;
    request.struct_size = sizeof(request);
    request.operation = operation;
    request.session = session;
    request.status = XSR_SERVICE_UNAVAILABLE;
    XPLMSendMessageToPlugin(1, XSR_MESSAGE_REQUEST, &request);
    return request;
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
    if (id == 2 && sender == 1 && message == XSR_MESSAGE_EVENT) {
        const auto& event = *static_cast<const XsrEvent*>(parameter);
        if (event.kind == XSR_CAPTURE_STARTED) captured_session = event.session;
        if (event.kind == XSR_RESULT && event.status == XSR_OK) {
            delivered_session = event.session;
            delivered_text = event.text;
            delivered_language = event.language;
        }
    }
    AsPlugin as(id);
    plugins[id].receive(sender, message, parameter);
}
void XPLMGetPrefsPath(char* value) { std::strcpy(value, prefs.c_str()); }
void XPLMDebugString(const char*) {}
XPLMLanguageCode XPLMGetLanguage() { return language; }
void XPLMGetVersions(int* xplane, int* sdk, XPLMHostApplicationID* host) {
    *xplane = 12440;
    *sdk = 440;
    *host = xplm_Host_XPlane;
}
float XPLMGetElapsedTime() { return std::chrono::duration<float>(Clock::now() - started).count(); }

XPLMDataRef XPLMRegisterDataAccessor(const char* name, XPLMDataTypeID type, int writable,
    XPLMGetDatai_f read, XPLMSetDatai_f write_int, XPLMGetDataf_f read_float, XPLMSetDataf_f write_float,
    XPLMGetDatad_f read_double, XPLMSetDatad_f write_double, XPLMGetDatavi_f read_int_array,
    XPLMSetDatavi_f write_int_array, XPLMGetDatavf_f read_float_array, XPLMSetDatavf_f write_float_array,
    XPLMGetDatab_f read_bytes, XPLMSetDatab_f write_bytes, void* read_context, void* write_context) {
    require(current == 1 && type == xplmType_Int && !writable && read && !write_int &&
        !read_float && !write_float && !read_double && !write_double && !read_int_array && !write_int_array &&
        !read_float_array && !write_float_array && !read_bytes && !write_bytes && !write_context,
        "dataref is a read-only integer");
    auto inserted = datarefs.emplace(name, RegisteredDataRef{read, read_context});
    require(inserted.second, "unique dataref");
    return reinterpret_cast<XPLMDataRef>(&inserted.first->second);
}

void XPLMUnregisterDataAccessor(XPLMDataRef handle) {
    const auto ref = std::find_if(datarefs.begin(), datarefs.end(), [&](auto& entry) {
        return reinterpret_cast<XPLMDataRef>(&entry.second) == handle;
    });
    require(ref != datarefs.end(), "dataref registered before removal");
    datarefs.erase(ref);
}

XPLMMenuID XPLMFindPluginsMenu() { return reinterpret_cast<XPLMMenuID>(plugins); }
std::map<int, std::string> menu_labels;
int XPLMAppendMenuItem(XPLMMenuID, const char* name, void*, int) { menu_labels[current] = name; return 0; }
void XPLMSetMenuItemName(XPLMMenuID, int, const char* name, int) { menu_labels[current] = name; }
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
void XPLMGetMouseLocationGlobal(int* x, int* y) { *x = mouse_x; *y = mouse_y; }
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* params) {
    auto& plugin = plugins[current];
    auto& window = params->decorateAsFloatingWindow == xplm_WindowDecorationRoundRectangle ? plugin.window : plugin.overlay;
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
void XPLMSetWindowGeometry(XPLMWindowID id, int left, int top, int right, int bottom) {
    auto& w = *reinterpret_cast<XPLMCreateWindow_t*>(id);
    w.left = left; w.top = top; w.right = right; w.bottom = bottom;
}
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
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
    vertices += mesh->vertex_count;
    for (int i = 0; i < count; ++i) {
        require(commands[i].tex_ref != nullptr && commands[i].element_count % 3 == 0, "draw call");
        ++drawn;
    }
}

int main(int argc, char** argv) {
    try {
        if (fs::path(argv[0]).filename() == "xsr-engine") return locale_test_engine();
        if (argc == 2 && std::string_view(argv[1]) == "--resize-grips") {
            Window floating([] {}, 365, 400, {}, xplm_WindowDecorationSelfDecorated);
            floating.enable_resize_grips();
            floating.set_bounds({895, 934, 1260, 534});
            auto& host = plugins[0].overlay;
            const auto id = reinterpret_cast<XPLMWindowID>(&host);
            host.handleMouseClickFunc(id, 901, 540, xplm_MouseDown, host.refcon);
            host.handleMouseClickFunc(id, 701, 390, xplm_MouseDrag, host.refcon);
            host.handleMouseClickFunc(id, 701, 390, xplm_MouseUp, host.refcon);
            require(floating.bounds() == Window::Bounds{695, 934, 1260, 384}, "left grip expands docked activity window");
            floating.set_bounds({360, 900, 800, 500});
            host.handleMouseClickFunc(id, 794, 506, xplm_MouseDown, host.refcon);
            host.handleMouseClickFunc(id, 914, 406, xplm_MouseDrag, host.refcon);
            host.handleMouseClickFunc(id, 914, 406, xplm_MouseUp, host.refcon);
            require(floating.bounds() == Window::Bounds{360, 900, 920, 400}, "right grip shares the top-left anchor");
            Window headings([&] {
                const auto& fonts = ImGui::GetIO().Fonts->Fonts;
                require(fonts.Size == 2 && fonts[0]->FontSize == 16 && fonts[1]->FontSize == 18,
                        "section headings have their own baked font size");
                headings.section_heading("Microphone indicator");
            }, 450, 260, {}, xplm_WindowDecorationRoundRectangle, true);
            auto& heading_host = plugins[0].window;
            const int before = drawn;
            heading_host.drawWindowFunc(reinterpret_cast<XPLMWindowID>(&heading_host), heading_host.refcon);
            require(drawn > before, "section headings render");
            std::cout << "Activity resize grips and section headings passed\n";
            return 0;
        }
        require(argc == 5, "provider diagnostic engine catalog");
        const auto root = fs::temp_directory_path() / ("xsr-native-" + std::to_string(
            Clock::now().time_since_epoch().count()));
        struct Fixture {
            fs::path root;
            ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
        } fixture{root};
        fs::create_directories(root / "bin");
        const bool locale_test = std::string_view(argv[3]) == "--locale-ptt";
        fs::create_symlink(fs::absolute(locale_test ? argv[0] : argv[3]), root / "bin/xsr-engine");
        fs::copy(fs::path(argv[4]).parent_path() / "locales", root / "locales", fs::copy_options::recursive);
        fs::copy_file(argv[4], root / "catalog.json");
        fs::copy_file(fs::path(argv[4]).parent_path() / "NotoSans-Regular.ttf", root / "NotoSans-Regular.ttf");
        prefs = (root / "X-Plane.prf").u8string();
        if (locale_test) {
            auto dir = root / "locales";
            Json registry = Json::parse(std::ifstream(dir / "catalog.json"));
            const auto english = Json::parse(std::ifstream(dir / "en.json"));
            InterfaceLocale shipped;
            for (const auto& [code, name] : registry.items()) {
                const auto data = Json::parse(std::ifstream(dir / (code + ".json")));
                require(data.size() == english.size(), "bundled catalog key count");
                shipped.initialize(dir, code);
                for (const auto& [key, reference] : english.items())
                    require(data.at(key) == shipped.lookup(key.c_str(), false), "complete valid shipped translation");
            }
            registry["it"] = "Italiano";
            registry["../escape"] = "Invalid path";
            std::ofstream(dir / "catalog.json") << registry;
            std::ofstream(dir / "it.json") << R"({"settings":"Impostazioni","show_diagnostic":"Apri diagnostica","no_requests":false,"seconds_remaining":"%n","close":"##invalid"})";
            InterfaceLocale locale;
            locale.initialize(dir, "it");
            require(!locale.languages().count("../escape") &&
                    std::string(locale.lookup("settings", true)) == "Impostazioni###settings" &&
                    std::string(locale.lookup("seconds_remaining", false)) == "%u s remaining" &&
                    std::string(locale.lookup("no_requests", false)) == "No requests" &&
                    std::string(locale.lookup("close", false)) == "Close", "partial catalog and format fallback");
            std::ofstream(dir / "it.json", std::ios::app) << "broken";
            require(!locale.refresh() && std::string(locale.lookup("settings", false)) == "Impostazioni",
                    "active locale remains cached");
            locale.initialize(dir, "it");
            require(std::string(locale.lookup("settings", false)) == "Settings", "malformed catalog");
            locale.initialize(root / "missing", "unknown");
            require(locale.preference() == "auto" && std::string(locale.lookup("settings", true)) == "Settings###settings",
                    "embedded English and stable widget identity");
            std::ofstream(dir / "it.json") << R"({"settings":"Impostazioni","show_diagnostic":"Apri diagnostica"})";

            load(1, argv[1], root / "64/lin.xpl");
            load(2, argv[2], root / "diagnostic/64/lin.xpl");
            auto until = [&](auto condition, const char* message) {
                const auto limit = Clock::now() + 3s;
                do {
                    pump(1);
                    pump(2);
                    if (condition()) return;
                    std::this_thread::sleep_for(5ms);
                } while (Clock::now() < limit);
                require(false, message);
            };
            until([] { return read_dataref(XSR_DATAREF_MODEL_STATE) == XSR_MODEL_READY; }, "fixture model ready");
            const auto session = ptt_from_diagnostic();
            require(session.status == XSR_OK, "PTT accepted");
            until([&] { return captured_session == session.session; }, "capture started");
            language = xplm_Language_Italian;
            draw(1);
            draw(2);
            require(menu_labels[1] == "Impostazioni" && menu_labels[2] == "Apri diagnostica", "fourth locale during capture");
            auto& settings_window = plugins[1].window;
            settings_window.bottom = settings_window.top - 1800;
            draw(1);
            draw(1);
            click(80, 1193);
            draw(1);
            click(80, 1313); // Français in the expanded interface-language selector.
            require(menu_labels[1] == "Réglages" && menu_labels[2] == "Afficher le diagnostic", "manual override reaches both plugins");
            language = xplm_Language_English;
            draw(1);
            draw(2);
            require(menu_labels[1] == "Réglages" && menu_labels[2] == "Afficher le diagnostic", "override survives simulator language change");
            require(read_dataref(XSR_DATAREF_PTT_BUSY) && read_dataref(XSR_DATAREF_SERVICE_READY), "PTT survives UI change");
            require(ptt_from_diagnostic(XSR_PTT_END, session.session).status == XSR_OK, "release original PTT");
            until([&] { return delivered_session == session.session; }, "original subscriber received the result");
            require(delivered_text == "Heading select." && delivered_language == "en", "transcript and ASR language preserved");
            for (int id : {2, 1}) {
                AsPlugin as(id);
                plugins[id].disable();
                plugins[id].enabled = false;
                plugins[id].stop();
                dlclose(plugins[id].library);
            }
            auto saved = Json::parse(std::ifstream(root / "x-speech-rekon.json"));
            require(saved.at("interface_language") == "fr" && saved.at("language") == "auto", "UI preference stored independently");
            load(1, argv[1], root / "64/lin.xpl");
            require(menu_labels[1] == "Réglages", "manual language restored on reload");
            {
                AsPlugin as(1);
                plugins[1].disable();
                plugins[1].enabled = false;
                plugins[1].stop();
                dlclose(plugins[1].library);
            }
            return 0;
        }
        const auto copy_tool = root / "bin/wl-copy";
        std::ofstream(copy_tool) << "#!/bin/sh\ncat > \"$XSR_TEST_CLIPBOARD\"\n";
        fs::permissions(copy_tool, fs::perms::owner_exec, fs::perm_options::add);
        const auto clipboard = root / "clipboard.txt";
        const char* inherited_path = std::getenv("PATH");
        const auto tool_path = (root / "bin").string() + ":" + (inherited_path ? inherited_path : "");
        require(setenv("PATH", tool_path.c_str(), 1) == 0 &&
                setenv("WAYLAND_DISPLAY", "xsr-test", 1) == 0 &&
                setenv("XSR_TEST_CLIPBOARD", clipboard.c_str(), 1) == 0, "isolated clipboard fixture");

        load(1, argv[1], root / "64/lin.xpl");
        load(2, argv[2], root / "diagnostic/64/lin.xpl");
        require(menu_labels[1] == "Réglages" && menu_labels[2] == "Afficher le diagnostic", "French menus");
        require(datarefs.size() == 3, "published datarefs");
        require(!read_dataref(XSR_DATAREF_SERVICE_READY), "offline before first engine handshake");
        wait_for_engine(true);
        require(read_dataref(XSR_DATAREF_MODEL_STATE) == XSR_MODEL_MISSING &&
                !read_dataref(XSR_DATAREF_PTT_BUSY), "ready engine with no model or PTT");
        draw(1);
        language = xplm_Language_English;
        draw(1);
        language = xplm_Language_Spanish;
        auto& provider_window = plugins[1].window;
        const int default_right = provider_window.right;
        const int default_bottom = provider_window.bottom;
        require(default_right - provider_window.left == 620, "compact provider window");
        provider_window.right = provider_window.left + 560;
        provider_window.bottom = provider_window.top - 460;
        draw(1);
        draw(2);
        require(menu_labels[1] == "Ajustes" && menu_labels[2] == "Mostrar diagnóstico", "menus follow simulator language");
        language = xplm_Language_English;
        draw(1);
        click(172, 49);
        vertices = 0;
        draw(1);
        const int empty_activity = vertices;
        require(ptt_from_diagnostic().status == XSR_MODEL_UNAVAILABLE, "diagnostic subscribed to provider");
        {
            AsPlugin as(2);
            require(plugins[2].command != nullptr, "assignable command");
            plugins[2].command(nullptr, xplm_CommandBegin, plugins[2].command_data);
            plugins[2].command(nullptr, xplm_CommandEnd, plugins[2].command_data);
        }
        draw(1);
        vertices = 0;
        draw(1);
        require(vertices > empty_activity, "Activity displays rejected requests");
        click(248, 49);
        draw(1);
        click(93, 91);
        const auto copy_deadline = Clock::now() + 2s;
        std::string report;
        do {
            std::ifstream file(clipboard);
            report.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (report.find("MODEL_UNAVAILABLE") != std::string::npos) break;
            std::this_thread::sleep_for(10ms);
        } while (Clock::now() < copy_deadline);
        require(report.find("X-Plane / SDK: 12440 / 440") != std::string::npos &&
                report.find("MODEL_UNAVAILABLE") != std::string::npos &&
                report.find(root.string()) == std::string::npos, "copied diagnostics contain only technical data");
        provider_window.right = default_right;
        provider_window.bottom = default_bottom;
        click(48, 50);
        draw(1);
        click(99, 590);
        require(!read_dataref(XSR_DATAREF_SERVICE_READY), "manual restart marks service offline");
        wait_for_engine();
        require(read_dataref(XSR_DATAREF_MODEL_STATE) == XSR_MODEL_MISSING &&
                !read_dataref(XSR_DATAREF_PTT_BUSY), "idle after restart");
        provider_window.right = provider_window.left + 560;
        provider_window.bottom = provider_window.top - 460;
        click(172, 49);
        draw(1);
        vertices = 0;
        draw(1);
        require(vertices > empty_activity, "manual restart preserves activity");
        require(ptt_from_diagnostic().status == XSR_MODEL_UNAVAILABLE, "manual restart preserves subscription");

        for (int id : {1, 2}) {
            AsPlugin as(id);
            auto& plug = plugins[id];
            plug.disable();
            plug.enabled = false;
            if (id == 1) {
                require(datarefs.size() == 3 && !read_dataref(XSR_DATAREF_SERVICE_READY) &&
                        read_dataref(XSR_DATAREF_MODEL_STATE) == XSR_MODEL_MISSING &&
                        !read_dataref(XSR_DATAREF_PTT_BUSY), "disabled provider reports defaults");
            }
            plug.stop();
            require(datarefs.empty(), "datarefs unregistered at stop");
            require(plug.flight == nullptr, "flight loop unregistered");
            dlclose(plug.library);
        }
        std::ofstream(root / "catalog.json") << "[{\"id\":\"incomplete-entry\"}]";
        std::ofstream(root / "x-speech-rekon.json")
            << R"({"show_micro_indicator":true,"show_micro_requester":true,"micro_opacity":50})";
        load(1, argv[1], root / "64/lin.xpl");
        wait_for_engine();
        draw(1);
        {
            AsPlugin as(1);
            auto& overlay = plugins[1].overlay;
            const auto id = reinterpret_cast<XPLMWindowID>(&overlay);
            require(overlay.visible && overlay.decorateAsFloatingWindow == xplm_WindowDecorationSelfDecorated,
                    "saved indicator opens as its own window");
            const int before = drawn;
            overlay.drawWindowFunc(id, overlay.refcon);
            require(drawn > before, "microphone indicator draws independently");
            const int old_left = overlay.left, old_top = overlay.top;
            const int x = old_left + 60, y = old_top - 20;
            overlay.handleMouseClickFunc(id, x, y, xplm_MouseDown, overlay.refcon);
            overlay.handleMouseClickFunc(id, x + 40, y + 25, xplm_MouseDrag, overlay.refcon);
            overlay.handleMouseClickFunc(id, x + 40, y + 25, xplm_MouseUp, overlay.refcon);
            require(overlay.left == old_left + 40 && overlay.top == old_top + 25, "microphone indicator drags");
        }
        {
            AsPlugin as(1);
            plugins[1].disable();
            plugins[1].enabled = false;
            plugins[1].stop();
            dlclose(plugins[1].library);
        }
        std::cout << "Native modules, PTT, datarefs, engine restart and ImGui drawing passed in the SDK test host\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
