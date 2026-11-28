#include "window.hpp"
#include "desktop.hpp"
#include "localization.hpp"

#include <XPLMPanelGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <utility>
#include <vector>

namespace xsl {

namespace {
constexpr int resize_grip_size = 18;
constexpr int max_window_extent = 32767;
constexpr std::size_t max_input_glyphs = 4096;
constexpr ImWchar base_glyphs[] = {0x0020, 0x052f, 0x2000, 0x206f, 0};
namespace fs = std::filesystem;
using FontFace = std::pair<fs::path, int>;

std::vector<FontFace> input_font_candidates(bool hangul) {
#ifdef _WIN32
    const auto* windows = _wgetenv(L"WINDIR");
    const fs::path fonts = (windows ? fs::path(windows) : fs::path(L"C:/Windows")) / "Fonts";
    if (hangul) return {{fonts / "malgun.ttf", 0}, {fonts / "gulim.ttc", 0}};
    return {{fonts / "meiryo.ttc", 0}, {fonts / "msgothic.ttc", 0},
            {fonts / "msyh.ttc", 0}, {fonts / "simsun.ttc", 0}};
#else
    const int face = hangul ? 1 : 0;
    std::vector<FontFace> fonts{
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", face},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", face},
        {"/usr/share/fonts/noto/NotoSansCJK-Regular.ttc", face}
    };
    if (!hangul) fonts.emplace_back("/usr/share/fonts/droid/DroidSansJapanese.ttf", 0);
    fonts.emplace_back("/usr/share/fonts/droid/DroidSansFallbackFull.ttf", 0);
    return fonts;
#endif
}

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
               XPLMWindowDecoration decoration, bool section_headings, int minimum_width)
    : draw_content_(std::move(draw)), fallback_font_(fallback_font), section_headings_(section_headings) {
    const auto previous = ImGui::GetCurrentContext();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(previous);
    {
        Context context(imgui_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        auto& platform = ImGui::GetPlatformIO();
        platform.Platform_ClipboardUserData = this;
        platform.Platform_SetClipboardTextFn = [](ImGuiContext* context, const char* text) {
            auto& window = *static_cast<Window*>(context->PlatformIO.Platform_ClipboardUserData);
            if (window.clipboard_copy_.valid()) {
                window.show_clipboard_error("could_not_copy_to_the_system_clipboard");
                return;
            }
            try {
                window.clipboard_copy_ = std::async(std::launch::async, copy_clipboard_text, std::string(text)).share();
            } catch (const std::exception&) {
                window.show_clipboard_error("could_not_copy_to_the_system_clipboard");
            }
        };
        platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* { return nullptr; };
        if (!fallback_font.empty()) {
            char simulator_path[1024]{};
            XPLMGetSystemPath(simulator_path);
            base_font_ = fs::u8path(simulator_path) / "Resources/fonts/Roboto-Regular.ttf";
        }
        build_fonts();
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
    if (window_ && minimum_width > 0)
        XPLMSetWindowResizingLimits(window_, minimum_width, 1, max_window_extent, max_window_extent);
    else if (window_ && decoration == xplm_WindowDecorationSelfDecoratedResizable)
        XPLMSetWindowResizingLimits(window_, 280, 150, 1100, 1000);
}

void Window::build_fonts() {
    auto& atlas = *ImGui::GetIO().Fonts;
    if (texture_) {
        XPLMDestroyTexture(texture_);
        texture_ = nullptr;
    }
    atlas.Clear();
    atlas.SetTexID(0);
    const auto add_font = [&](float size) {
        ImFontConfig config;
        config.SizePixels = size;
        for (const auto* path : {&base_font_, &fallback_font_}) {
            std::error_code error;
            if (path->empty() || !fs::is_regular_file(*path, error)) continue;
            if (auto* font = atlas.AddFontFromFileTTF(path->u8string().c_str(), size, &config, base_glyphs))
                return font;
        }
        return atlas.AddFontDefault(&config);
    };
    add_font(16);
    for (const auto& input : input_fonts_) {
        if (input.font.first.empty() || input.ranges.empty()) continue;
        ImFontConfig config;
        config.MergeMode = true;
        config.FontNo = input.font.second;
        atlas.AddFontFromFileTTF(input.font.first.u8string().c_str(), 16, &config, input.ranges.Data);
    }
    if (section_headings_) heading_font_ = add_font(18);
}

void Window::include_glyphs(std::string_view text) {
    if (text.empty()) return;
    const auto* end = text.data() + text.size();
    for (const auto* current = text.data(); current < end;) {
        unsigned codepoint = 0;
        current += ImTextCharFromUtf8(&codepoint, current, end);
        const bool hangul = (codepoint >= 0x1100 && codepoint <= 0x11ff) ||
            (codepoint >= 0x3130 && codepoint <= 0x318f) || (codepoint >= 0xa960 && codepoint <= 0xa97f) ||
            (codepoint >= 0xac00 && codepoint <= 0xd7ff);
        const bool east_asian = (codepoint >= 0x2e80 && codepoint <= 0x9fff) ||
            (codepoint >= 0xf900 && codepoint <= 0xfaff) || (codepoint >= 0xff00 && codepoint <= 0xffef) ||
            (codepoint >= 0x20000 && codepoint <= 0x323af);
        if (!hangul && !east_asian) continue;
        auto& input = input_fonts_[hangul ? 1 : 0];
        if (input.glyphs.size() < max_input_glyphs && input.glyphs.insert(codepoint).second)
            input_fonts_dirty_ = true;
    }
}

void Window::update_input_fonts() {
    if (!input_fonts_dirty_) return;
    input_fonts_dirty_ = false;
    bool rebuild = false;
    for (std::size_t index = 0; index < input_fonts_.size(); ++index) {
        auto& input = input_fonts_[index];
        if (input.glyphs.empty()) continue;
        if (input.font.first.empty()) {
            for (const auto& candidate : input_font_candidates(index == 1)) {
                std::error_code error;
                if (!fs::is_regular_file(candidate.first, error)) continue;
                input.font = candidate;
                break;
            }
        }
        if (input.font.first.empty()) continue;
        input.ranges.clear();
        for (const auto glyph : input.glyphs) {
            if (!input.ranges.empty() && input.ranges.back() + 1 == glyph) input.ranges.back() = glyph;
            else {
                input.ranges.push_back(glyph);
                input.ranges.push_back(glyph);
            }
        }
        input.ranges.push_back(0);
        rebuild = true;
    }
    if (rebuild) build_fonts();
}

void Window::edit_text(const char* label, const char* hint, std::string& value) {
    const auto paste = [](ImGuiInputTextCallbackData* data) -> int {
        const auto& source = *static_cast<const std::string*>(data->UserData);
        std::string text;
        text.reserve(source.size());
        const auto* end = source.data() + source.size();
        for (const auto* current = source.data(); current < end;) {
            unsigned codepoint = 0;
            const auto length = ImTextCharFromUtf8(&codepoint, current, end);
            if (codepoint == IM_UNICODE_CODEPOINT_INVALID) text += "\xef\xbf\xbd";
            else if (codepoint >= 32 && codepoint != 127) text.append(current, length);
            current += length;
        }
        if (text.empty()) return 0;
        const auto selected = std::abs(data->SelectionEnd - data->SelectionStart);
        const auto start = selected ? std::min(data->SelectionStart, data->SelectionEnd) : data->CursorPos;
        if (selected) data->DeleteChars(start, selected);
        data->CursorPos = start;
        data->InsertChars(start, text.data(), text.data() + text.size());
        return 0;
    };
    include_glyphs(value);
    const bool enabled = !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
    const auto flags = enabled && paste_text_ && paste_field_ == ImGui::GetID(label) ?
        ImGuiInputTextFlags_CallbackAlways : ImGuiInputTextFlags_None;
    if (ImGui::InputTextWithHint(label, hint, &value, flags, paste, paste_text_ ? &*paste_text_ : nullptr))
        include_glyphs(value);
}

void Window::start_paste() {
    if (clipboard_read_.valid() || paste_field_) return;
    const auto field = ImGui::GetActiveID();
    if (!ImGui::GetInputTextState(field)) return;
    try {
        clipboard_read_ = std::async(std::launch::async, [copy = clipboard_copy_]() -> std::optional<std::string> {
            if (copy.valid() && !copy.get()) return std::nullopt;
            return paste_clipboard_text();
        });
        paste_field_ = field;
    } catch (const std::exception&) {
        show_clipboard_error("could_not_paste_field_text");
    }
}

void Window::poll_clipboard() {
    using namespace std::chrono_literals;
    if (clipboard_copy_.valid() && clipboard_copy_.wait_for(0s) == std::future_status::ready) {
        bool copied = false;
        try { copied = clipboard_copy_.get(); }
        catch (const std::exception&) {}
        clipboard_copy_ = {};
        if (!copied) show_clipboard_error("could_not_copy_to_the_system_clipboard");
    }
    if (!clipboard_read_.valid() || clipboard_read_.wait_for(0s) != std::future_status::ready) return;
    std::optional<std::string> text;
    try { text = clipboard_read_.get(); }
    catch (const std::exception&) {}
    if (!paste_field_ || paste_field_ != ImGui::GetActiveID()) {
        paste_field_ = 0;
    } else if (text) {
        include_glyphs(*text);
        paste_text_ = std::move(text);
        clipboard_notice_ = nullptr;
    } else {
        paste_field_ = 0;
        show_clipboard_error("could_not_paste_field_text");
    }
}

void Window::show_clipboard_error(const char* key) {
    clipboard_notice_ = key;
    clipboard_notice_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

void Window::draw_clipboard_notice() {
    if (!clipboard_notice_ || std::chrono::steady_clock::now() >= clipboard_notice_until_) return;
    const auto size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({size.x - 12, size.y - 12}, ImGuiCond_Always, {1, 1});
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##clipboard-notice", nullptr, flags)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::clamp(size.x - 56.f, 1.f, 360.f));
        ImGui::TextUnformatted(tr(clipboard_notice_));
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

Window::~Window() {
    if (keyboard_focused_) XPLMTakeKeyboardFocus(nullptr);
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
    if (keyboard_focused_) XPLMTakeKeyboardFocus(nullptr);
    keyboard_focused_ = false;
    Context context(imgui_);
    mouse_down_ = false;
    dragging_ = false;
    resize_direction_ = 0;
    paste_field_ = 0;
    paste_text_.reset();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    XPLMSetWindowIsVisible(window_, 0);
}

void Window::position(int x, int y) {
    int left, top, right, bottom;
    XPLMGetWindowGeometry(window_, &left, &top, &right, &bottom);
    ImGui::GetIO().AddMousePosEvent(float(x - left), float(top - y));
}

void Window::key_cb(XPLMWindowID, char character, XPLMKeyFlags flags, char key, void* data, int losing_focus) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    auto& io = ImGui::GetIO();
    if (losing_focus) {
        self.keyboard_focused_ = false;
        self.paste_field_ = 0;
        self.paste_text_.reset();
        io.AddFocusEvent(false);
        return;
    }
    const bool down = !(flags & xplm_UpFlag);
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & xplm_ControlFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag) != 0);
    if (down && key == 'V' && (flags & xplm_ControlFlag) && !(flags & (xplm_OptionAltFlag | xplm_ShiftFlag))) {
        self.start_paste();
        return;
    }
    constexpr std::pair<int, ImGuiKey> keys[] = {
        {XPLM_VK_BACK, ImGuiKey_Backspace}, {XPLM_VK_DELETE, ImGuiKey_Delete},
        {XPLM_VK_LEFT, ImGuiKey_LeftArrow}, {XPLM_VK_RIGHT, ImGuiKey_RightArrow},
        {XPLM_VK_HOME, ImGuiKey_Home}, {XPLM_VK_END, ImGuiKey_End},
        {XPLM_VK_RETURN, ImGuiKey_Enter}, {XPLM_VK_ESCAPE, ImGuiKey_Escape}, {XPLM_VK_TAB, ImGuiKey_Tab},
        {'A', ImGuiKey_A}, {'C', ImGuiKey_C}, {'V', ImGuiKey_V}, {'X', ImGuiKey_X},
        {'Y', ImGuiKey_Y}, {'Z', ImGuiKey_Z}
    };
    for (const auto& [code, imgui_key] : keys)
        if (code == static_cast<unsigned char>(key)) io.AddKeyEvent(imgui_key, down);
    const auto typed = static_cast<unsigned char>(character);
    if (down && (!(flags & xplm_ControlFlag) || (flags & xplm_OptionAltFlag)) && typed >= 32 && typed != 127)
        io.AddInputCharacter(typed);
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
    poll_clipboard();
    update_input_fonts();
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
    ImGui::Begin("##xsl", nullptr, flags);
    try { draw_content_(); }
    catch (const std::exception&) {
        const bool recovery_asserts = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::EndFrame();
        io.ConfigErrorRecoveryEnableAssert = recovery_asserts;
        throw;
    }
    ImGui::End();
    if (paste_text_ || paste_field_ != ImGui::GetActiveID()) paste_field_ = 0;
    paste_text_.reset();
    draw_clipboard_notice();
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
    if (io.WantTextInput != keyboard_focused_) {
        keyboard_focused_ = io.WantTextInput;
        XPLMTakeKeyboardFocus(keyboard_focused_ ? window_ : nullptr);
        if (keyboard_focused_) io.AddFocusEvent(true);
    }

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

} // namespace xsl
