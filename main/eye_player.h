#pragma once

#include "gif/lvgl_gif.h"
#include "lvgl_image.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class EyePlayer {
public:
    static EyePlayer& GetInstance() {
        static EyePlayer instance;
        return instance;
    }

    void Start();
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(); }
    /**
     * Switch expression.
     * @param auto_return if true, non-blink clips return to blink after one play;
     *                    if false, hold until next switch (BOOT / serial / MCP).
     */
    bool SetExpr(const std::string& expr, bool force = false, bool auto_return = true);
    bool SetExprByIndex(int index, bool auto_return = false);
    bool SetExprFromEmotion(const std::string& emotion);
    std::string CycleNext();
    void RequestCycleNext();
    std::string CurrentExpr() const;
    size_t ExprCount() const;
    const char* ExprName(size_t index) const;
    const char* ExprLabelZh(size_t index) const;
    void PrintExprList() const;

private:
    EyePlayer();
    ~EyePlayer();

    void RegisterMcpTools();
    void StartSerialCli();
    bool LoadClip(const std::string& name);
    void OnFrame();
    void ScheduleReturnToBlink(uint32_t delay_ms);
    void CancelReturnTimer();
    static void ReturnToBlinkTimer(void* arg);
    static void SerialCliTask(void* arg);
    void HandleSerialLine(const char* line, bool& interactive);

    std::atomic<bool> started_{false};
    std::atomic<bool> enabled_{true};
    std::string current_expr_;
    std::unique_ptr<LvglRawImage> asset_image_;
    std::unique_ptr<LvglGif> gif_;
    esp_timer_handle_t return_timer_ = nullptr;
    TaskHandle_t cli_task_ = nullptr;
};
