#include "window.hpp"

#include <XPLMPanelGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

namespace xsr {

const char* localized(const char* english, const char* french, const char* spanish) {
    switch (XPLMGetLanguage()) {
    case xplm_Language_French: return french;
    case xplm_Language_Spanish: return spanish;
    default: return english;
    }
}

namespace {
class Context {
public:
    explicit Context(ImGuiContext* context) : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
    ~Context() { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};
}

Window::Window(std::function<void()> draw, int width, int height, const std::filesystem::path& fallback_font)
    : draw_content_(std::move(draw)) {
    const auto previous = ImGui::GetCurrentContext();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(previous);
    {
        Context context(imgui_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        ImFontConfig font;
        font.SizePixels = 16;
        if (!fallback_font.empty()) {
            char simulator_path[1024]{};
            XPLMGetSystemPath(simulator_path);
            const auto roboto = std::filesystem::u8path(simulator_path) / "Resources/fonts/Roboto-Regular.ttf";
            static constexpr ImWchar ranges[] = {0x0020, 0x052f, 0x2000, 0x206f, 0};
            for (const auto& path : {roboto, fallback_font}) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error) &&
                    io.Fonts->AddFontFromFileTTF(path.u8string().c_str(), font.SizePixels, &font, ranges)) break;
            }
        }
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault(&font);
        auto& style = ImGui::GetStyle();
        style.WindowPadding = {16, 14};
        style.FramePadding = {9, 5};
        style.ItemSpacing = {9, 8};
        style.CellPadding = {10, 6};
        style.FrameRounding = 3;
        style.WindowRounding = 0;
        style.Colors[ImGuiCol_WindowBg] = {0.14f, 0.14f, 0.15f, 1};
        style.Colors[ImGuiCol_ChildBg] = {0.10f, 0.10f, 0.11f, 1};
        style.Colors[ImGuiCol_Text] = {0.95f, 0.96f, 0.98f, 1};
        style.Colors[ImGuiCol_TextDisabled] = {0.64f, 0.67f, 0.72f, 1};
        style.Colors[ImGuiCol_Header] = {0.18f, 0.27f, 0.40f, 1};
        style.Colors[ImGuiCol_Button] = {0.19f, 0.29f, 0.44f, 1};
    }

    int left, top, right, bottom;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
    XPLMCreateWindow_t params{};
    params.structSize = sizeof(params);
    params.left = left + 55;
    params.top = top - 75;
    params.right = std::min(right - 12, params.left + width);
    params.bottom = std::max(bottom + 12, params.top - height);
    params.visible = 0;
    params.drawWindowFunc = draw_cb;
    params.handleMouseClickFunc = click_cb;
    params.handleKeyFunc = key_cb;
    params.handleCursorFunc = cursor_cb;
    params.handleMouseWheelFunc = wheel_cb;
    params.refcon = this;
    params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    params.layer = xplm_WindowLayerFloatingWindows;
    params.contentType = xplm_WindowContentTypePanelGraphics;
    window_ = XPLMCreateWindowEx(&params);
}

Window::~Window() {
    if (window_) XPLMDestroyWindow(window_);
    if (texture_) XPLMDestroyTexture(texture_);
    if (imgui_) ImGui::DestroyContext(imgui_);
}

bool Window::visible() const { return window_ && XPLMGetWindowIsVisible(window_); }

void Window::show(const char* title) {
    if (!window_) return;
    XPLMSetWindowTitle(window_, title);
    XPLMSetWindowIsVisible(window_, 1);
    XPLMBringWindowToFront(window_);
}

void Window::hide() {
    if (!window_) return;
    Context context(imgui_);
    mouse_down_ = false;
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    XPLMSetWindowIsVisible(window_, 0);
}

void Window::position(int x, int y) {
    int left, top, right, bottom;
    XPLMGetWindowGeometry(window_, &left, &top, &right, &bottom);
    ImGui::GetIO().AddMousePosEvent(float(x - left), float(top - y));
}

void Window::draw_cb(XPLMWindowID, void* data) {
    try { static_cast<Window*>(data)->draw(); }
    catch (const std::exception&) { XPLMDebugString("X-Speech Rekon: window rendering failed\n"); }
}

int Window::click_cb(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* data) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    self.position(x, y);
    if (status != xplm_MouseDrag) {
        self.mouse_down_ = status == xplm_MouseDown;
        ImGui::GetIO().AddMouseButtonEvent(0, self.mouse_down_);
    }
    return 1;
}

int Window::wheel_cb(XPLMWindowID, int x, int y, int wheel, int clicks, void* data) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    self.position(x, y);
    ImGui::GetIO().AddMouseWheelEvent(wheel == 1 ? float(clicks) : 0, wheel == 0 ? float(clicks) : 0);
    return 1;
}

void Window::draw() {
    Context context(imgui_);
    auto& io = ImGui::GetIO();
    if (!texture_) {
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        texture_ = XPLMCreateTexture(pixels, width, height);
        if (!texture_) return;
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(texture_));
    }
    int left, top, right, bottom, x, y;
    XPLMGetWindowGeometry(window_, &left, &top, &right, &bottom);
    XPLMGetMouseLocationGlobal(&x, &y);
    position(x, y);
    io.DisplaySize = {float(right - left), float(top - bottom)};
    io.DeltaTime = std::max(0.001f, XPLMGetElapsedTime() - frame_time_);
    frame_time_ = XPLMGetElapsedTime();
    io.DeltaTime = std::min(io.DeltaTime, 0.1f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr auto flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##xsr", nullptr, flags);
    try { draw_content_(); }
    catch (const std::exception&) {
        const bool recovery_asserts = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::EndFrame();
        io.ConfigErrorRecoveryEnableAssert = recovery_asserts;
        throw;
    }
    ImGui::End();
    ImGui::Render();

    const auto* data = ImGui::GetDrawData();
    static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, col) == 16);
    static_assert(sizeof(ImDrawIdx) == sizeof(uint16_t));
    XPLMTransformPush();
    struct RestoreTransform { ~RestoreTransform() { XPLMTransformPop(); } } restore_transform;
    XPLMTransformTranslate(float(left), float(top));
    XPLMTransformScale(1.0f, -1.0f);
    std::vector<XPLMDrawCall_t> calls;
    for (int i = 0; i < data->CmdListsCount; ++i) {
        const auto& list = *data->CmdLists[i];
        calls.clear();
        for (const auto& command : list.CmdBuffer) {
            if (command.UserCallback) continue;
            calls.push_back({reinterpret_cast<void*>(command.GetTexID()),
                {command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w},
                int(command.IdxOffset), int(command.ElemCount), int(command.VtxOffset)});
        }
        XPLMMesh_t mesh{list.VtxBuffer.Size, reinterpret_cast<const float*>(list.VtxBuffer.Data),
                        list.IdxBuffer.Size, list.IdxBuffer.Data};
        XPLMDrawCalls(&mesh, int(calls.size()), calls.data());
    }
}

} // namespace xsr
