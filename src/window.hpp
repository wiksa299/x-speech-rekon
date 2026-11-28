#pragma once

#include <XPLMDisplay.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace xsl {


inline constexpr ImGuiChildFlags settings_panel_flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY;
void push_settings_style();
void pop_settings_style();

class Window {
public:
    using Bounds = std::array<int, 4>;
    explicit Window(std::function<void()> draw, int width = 690, int height = 680,
                    const std::filesystem::path& fallback_font = {},
                    XPLMWindowDecoration decoration = xplm_WindowDecorationRoundRectangle,
                    bool section_headings = false, int minimum_width = 0);
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
    void include_glyphs(std::string_view text);
    void edit_text(const char* label, const char* hint, std::string& value);
    void set_font_scale(float scale) { font_scale_ = scale; }
    void set_drag_area(int height, int reserved_right = 0) { drag_height_ = height; reserved_right_ = reserved_right; }
    void enable_resize_grips() { resize_grips_ = true; }
    void set_resize_grips_visible(bool visible) { show_resize_grips_ = visible; }

private:
    static void draw_cb(XPLMWindowID window, void* data);
    static int click_cb(XPLMWindowID window, int x, int y, XPLMMouseStatus status, void* data);
    static int wheel_cb(XPLMWindowID window, int x, int y, int wheel, int clicks, void* data);
    static void key_cb(XPLMWindowID, char character, XPLMKeyFlags flags, char key, void* data, int losing_focus);
    static XPLMCursorStatus cursor_cb(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
    void position(int x, int y);
    void resize_from_grip(int x, int y);
    void build_fonts();
    void update_input_fonts();
    void start_paste();
    void poll_clipboard();
    void show_clipboard_error(const char* key);
    void draw_clipboard_notice();
    void draw();

    struct InputFont {
        std::set<ImWchar> glyphs;
        ImVector<ImWchar> ranges;
        std::pair<std::filesystem::path, int> font;
    };
    std::function<void()> draw_content_;
    std::filesystem::path base_font_, fallback_font_;
    bool section_headings_;
    std::array<InputFont, 2> input_fonts_;
    bool input_fonts_dirty_ = false;
    XPLMWindowID window_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    ImFont* heading_font_ = nullptr;
    std::shared_future<bool> clipboard_copy_;
    std::future<std::optional<std::string>> clipboard_read_;
    std::optional<std::string> paste_text_;
    ImGuiID paste_field_ = 0;
    const char* clipboard_notice_ = nullptr;
    std::chrono::steady_clock::time_point clipboard_notice_until_{};
    void* texture_ = nullptr;
    bool mouse_down_ = false;
    bool keyboard_focused_ = false;
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

} // namespace xsl
