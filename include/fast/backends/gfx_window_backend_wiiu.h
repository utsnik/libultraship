#ifdef ENABLE_GX2
#pragma once

#include <stdint.h>
#include "gfx_window_manager_api.h"

namespace Fast {

/**
 * @brief Wii U window/proc backend for Fast3D.
 *
 * Replaces the old C `GfxWindowManagerAPI` struct. The Wii U has no mouse and no
 * window manager: the framebuffer is fixed (TV + GamePad), fullscreen is the only
 * state, and the process lifetime is driven by ProcUI rather than an event queue.
 * The pointer-related methods are therefore honest no-ops rather than fabricated
 * values -- GamePad touch is delivered through the ImGui Wii U backend instead.
 */
class GfxWindowBackendWiiU : public GfxWindowBackend {
  public:
    GfxWindowBackendWiiU() = default;
    ~GfxWindowBackendWiiU() override = default;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void Destroy() override;

    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool isNowFullscreen)) override;

    void SetFullscreen(bool fullscreen) override;
    bool IsFullscreen() override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visible) override;

    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;

    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    // SetDimensions()/GetPrimaryMonitorRect() are newer-LUS virtuals absent from the
    // 9.2.3 GfxWindowBackend; the Wii U has fixed scan-out so neither had any work to do.

    void HandleEvents() override;
    bool IsFrameReady() override;
    bool IsRunning() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;

    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;

    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;

  private:
    int mTargetFps = 60;
};
} // namespace Fast
#endif
