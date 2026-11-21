#pragma once

#include <XPLMDisplay.h>
#include <imgui.h>

#include <filesystem>
#include <functional>

namespace xsr {

const char* localized(const char* english, const char* french, const char* spanish);

class Window {
public:
    explicit Window(std::function<void()> draw, int width = 690, int height = 680,
                    const std::filesystem::path& fallback_font = {});
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void show(const char* title);
    void hide();
    bool visible() const;
    bool pointer_pressed() const { return mouse_down_; }

private:
    static void draw_cb(XPLMWindowID window, void* data);
    static int click_cb(XPLMWindowID window, int x, int y, XPLMMouseStatus status, void* data);
    static int wheel_cb(XPLMWindowID window, int x, int y, int wheel, int clicks, void* data);
    static void key_cb(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
    static XPLMCursorStatus cursor_cb(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
    void position(int x, int y);
    void draw();

    std::function<void()> draw_content_;
    XPLMWindowID window_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    void* texture_ = nullptr;
    bool mouse_down_ = false;
    float frame_time_ = 0;
};

} // namespace xsr
