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

namespace {
constexpr int resize_grip_size = 18;
class Context {
public:
    explicit Context(ImGuiContext* context) : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
    ~Context() { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};
}

void push_settings_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.164f, 0.176f, 0.199f, 1});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.16f, 0.176f, 0.199f, 1});
    ImGui::PushStyleColor(ImGuiCol_Border, {0.25f, 0.27f, 0.30f, 1});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.207f, 0.226f, 0.267f, 1});
    ImGui::PushStyleColor(ImGuiCol_Button, {0.205f, 0.256f, 0.330f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.23f, 0.45f, 0.68f, 1});
    ImGui::PushStyleColor(ImGuiCol_Tab, {0.165f, 0.187f, 0.225f, 1});
    ImGui::PushStyleColor(ImGuiCol_TabSelected, {0.21f, 0.32f, 0.48f, 1});
}

void pop_settings_style() {
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(4);
}

Window::Window(std::function<void()> draw, int width, int height, const std::filesystem::path& fallback_font,
               XPLMWindowDecoration decoration, bool section_headings)
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
        static constexpr ImWchar ranges[] = {0x0020, 0x052f, 0x2000, 0x206f, 0};
        std::filesystem::path font_path;
        if (!fallback_font.empty()) {
            char simulator_path[1024]{};
            XPLMGetSystemPath(simulator_path);
            const auto roboto = std::filesystem::u8path(simulator_path) / "Resources/fonts/Roboto-Regular.ttf";
            for (const auto& path : {roboto, fallback_font}) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error) &&
                    io.Fonts->AddFontFromFileTTF(path.u8string().c_str(), font.SizePixels, &font, ranges)) {
                    font_path = path;
                    break;
                }
            }
        }
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault(&font);
        if (section_headings) {
            font.SizePixels = 18;
            heading_font_ = font_path.empty() ? io.Fonts->AddFontDefault(&font) :
                io.Fonts->AddFontFromFileTTF(font_path.u8string().c_str(), font.SizePixels, &font, ranges);
        }
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
    params.decorateAsFloatingWindow = decoration;
    params.layer = xplm_WindowLayerFloatingWindows;
    params.contentType = xplm_WindowContentTypePanelGraphics;
    window_ = XPLMCreateWindowEx(&params);
    if (window_ && decoration == xplm_WindowDecorationSelfDecoratedResizable)
        XPLMSetWindowResizingLimits(window_, 280, 150, 1100, 1000);
}

Window::~Window() {
    if (window_) XPLMDestroyWindow(window_);
    if (texture_) XPLMDestroyTexture(texture_);
    if (imgui_) ImGui::DestroyContext(imgui_);
}

bool Window::visible() const { return window_ && XPLMGetWindowIsVisible(window_); }

void Window::set_border_opacity(float opacity) {
    Context context(imgui_);
    ImGui::GetStyle().Colors[ImGuiCol_Border].w = std::clamp(opacity, 0.f, 1.f);
}

void Window::set_padding(ImVec2 padding) {
    Context context(imgui_);
    ImGui::GetStyle().WindowPadding = padding;
}

void Window::section_heading(const char* text) const {
    ImGui::PushFont(heading_font_);
    ImGui::PushStyleColor(ImGuiCol_Text, {0.50f, 0.74f, 1.0f, 1.0f});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

Window::Bounds Window::bounds() const {
    Bounds result{};
    if (window_) XPLMGetWindowGeometry(window_, &result[0], &result[1], &result[2], &result[3]);
    return result;
}

void Window::set_bounds(Bounds value) {
    if (!window_) return;
    int left, top, right, bottom;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
    if (right <= left || top <= bottom) return;
    const auto width = std::clamp(static_cast<long long>(value[2]) - value[0], 0LL, static_cast<long long>(right - left));
    const auto height = std::clamp(static_cast<long long>(value[1]) - value[3], 0LL, static_cast<long long>(top - bottom));
    if (!width || !height) return;
    const int x = std::clamp(value[0], left, right - static_cast<int>(width));
    const int y = std::clamp(value[1], bottom + static_cast<int>(height), top);
    XPLMSetWindowGeometry(window_, x, y, x + static_cast<int>(width), y - static_cast<int>(height));
}

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
    dragging_ = false;
    resize_direction_ = 0;
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
    catch (const std::exception&) { XPLMDebugString("XPLM ImGui window rendering failed\n"); }
}

void Window::resize_from_grip(int x, int y) {
    int left, top, right, bottom;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
    auto area = resize_start_;
    const bool from_left = resize_direction_ < 0;
    const int max_width = std::max(1, std::min(1100, from_left ? area[2] - left : right - area[0]));
    const int max_height = std::max(1, std::min(1000, area[1] - bottom));
    const int width = std::clamp(area[2] - area[0] + resize_direction_ * (x - drag_x_), std::min(280, max_width), max_width);
    const int height = std::clamp(area[1] - area[3] - (y - drag_y_), std::min(150, max_height), max_height);
    if (from_left) area[0] = area[2] - width;
    else area[2] = area[0] + width;
    area[3] = area[1] - height;
    set_bounds(area);
}

int Window::click_cb(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* data) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    if (status == xplm_MouseDown) {
        const auto area = self.bounds();
        self.resize_direction_ = 0;
        if (self.resize_grips_ && y >= area[3] && y < area[3] + resize_grip_size) {
            if (x >= area[0] && x < area[0] + resize_grip_size) self.resize_direction_ = -1;
            else if (x <= area[2] && x > area[2] - resize_grip_size) self.resize_direction_ = 1;
        }
        self.resize_start_ = area;
        self.dragging_ = !self.resize_direction_ && self.drag_height_ && y <= area[1] && y > area[1] - self.drag_height_ &&
                         x >= area[0] && x < area[2] - self.reserved_right_;
        self.drag_x_ = x;
        self.drag_y_ = y;
    } else if (status == xplm_MouseDrag && self.resize_direction_) {
        self.resize_from_grip(x, y);
    } else if (status == xplm_MouseDrag && self.dragging_) {
        auto area = self.bounds();
        area[0] += x - self.drag_x_;
        area[2] += x - self.drag_x_;
        area[1] += y - self.drag_y_;
        area[3] += y - self.drag_y_;
        self.set_bounds(area);
        self.drag_x_ = x;
        self.drag_y_ = y;
    } else if (status == xplm_MouseUp) {
        self.dragging_ = false;
        if (self.resize_direction_) self.resize_from_grip(x, y);
        self.resize_direction_ = 0;
    }
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
    io.FontGlobalScale = font_scale_;
    io.DeltaTime = std::max(0.001f, XPLMGetElapsedTime() - frame_time_);
    frame_time_ = XPLMGetElapsedTime();
    io.DeltaTime = std::min(io.DeltaTime, 0.1f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(opacity_);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
    if (!scroll_enabled_) flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
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
    if (resize_grips_ && show_resize_grips_) {
        auto* draw = ImGui::GetForegroundDrawList();
        const auto line = [&](int side, float offset) {
            const float x = side < 0 ? 4.f : io.DisplaySize.x - 4.f;
            const float y = io.DisplaySize.y - 4.f;
            const ImU32 color = ImGui::GetColorU32(side == resize_direction_ ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            draw->AddLine({x, y - offset}, {x - side * offset, y}, color, 1.3f);
        };
        for (int side : {-1, 1}) {
            line(side, 10.f);
            line(side, 5.f);
        }
    }
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
