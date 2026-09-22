#pragma once

#include "gif/lvgl_gif.h"
#include "lvgl_image.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <esp_timer.h>

class EyePlayer {
public:
    static EyePlayer& GetInstance() {
        static EyePlayer instance;
        return instance;
    }

    void Start();
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(); }
    bool SetExpr(const std::string& expr, bool force = false);
    std::string CurrentExpr() const;

private:
    EyePlayer();
    ~EyePlayer();

    void RegisterMcpTools();
    bool LoadClip(const std::string& name);
    void OnFrame();
    void ScheduleReturnToBlink(uint32_t delay_ms);
    void CancelReturnTimer();
    static void ReturnToBlinkTimer(void* arg);

    std::atomic<bool> started_{false};
    std::atomic<bool> enabled_{true};
    std::string current_expr_;
    std::unique_ptr<LvglRawImage> asset_image_;
    std::unique_ptr<LvglGif> gif_;
    esp_timer_handle_t return_timer_ = nullptr;
};
