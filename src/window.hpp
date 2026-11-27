#pragma once

#include <XPLMDisplay.h>
#include <imgui.h>

#include <array>
#include <filesystem>
#include <functional>

namespace xsr {


inline constexpr ImGuiChildFlags settings_panel_flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY;
void push_settings_style();
void pop_settings_style();

class Window {
public:
    using Bounds = std::array<int, 4>;
    explicit Window(std::function<void()> draw, int width = 690, int height = 680,
                    const std::filesystem::path& fallback_font = {},
                    XPLMWindowDecoration decoration = xplm_WindowDecorationRoundRectangle,
                    bool section_headings = false);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void show(const char* title);
    void hide();
    bool visible() const;
    bool pointer_pressed() const { return mouse_down_; }
    Bounds bounds() const;
    void set_bounds(Bounds value);
    void set_background_opacity(float opacity) { opacity_ = opacity; }
    void set_border_opacity(float opacity);
    void set_padding(ImVec2 padding);
    void set_scroll_enabled(bool enabled) { scroll_enabled_ = enabled; }
    void section_heading(const char* text) const;
    void set_font_scale(float scale) { font_scale_ = scale; }
    void set_drag_area(int height, int reserved_right = 0) { drag_height_ = height; reserved_right_ = reserved_right; }
    void enable_resize_grips() { resize_grips_ = true; }
    void set_resize_grips_visible(bool visible) { show_resize_grips_ = visible; }

private:
    static void draw_cb(XPLMWindowID window, void* data);
    static int click_cb(XPLMWindowID window, int x, int y, XPLMMouseStatus status, void* data);
    static int wheel_cb(XPLMWindowID window, int x, int y, int wheel, int clicks, void* data);
    static void key_cb(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
    static XPLMCursorStatus cursor_cb(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
    void position(int x, int y);
    void resize_from_grip(int x, int y);
    void draw();

    std::function<void()> draw_content_;
    XPLMWindowID window_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    ImFont* heading_font_ = nullptr;
    void* texture_ = nullptr;
    bool mouse_down_ = false;
    bool dragging_ = false;
    bool resize_grips_ = false;
    bool show_resize_grips_ = true;
    bool scroll_enabled_ = true;
    int resize_direction_ = 0;
    Bounds resize_start_{};
    int drag_height_ = 0, reserved_right_ = 0, drag_x_ = 0, drag_y_ = 0;
    float opacity_ = 1.0f, font_scale_ = 1.0f;
    float frame_time_ = 0;
};

} // namespace xsr
