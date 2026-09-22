#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_EYE_ANIM

#include "eye_player.h"

#include "assets.h"
#include "board.h"
#include "display.h"
#include "gif/lvgl_gif.h"
#include "lvgl_image.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>

#include <cstring>

#define TAG "EyePlayer"

namespace {

constexpr uint32_t kClipDurationMs = 4010;  // ~48 frames @ 12fps
constexpr uint32_t kHappyExcitedLoops = 2;

const char* AssetNameForExpr(const std::string& expr) {
    if (expr == "happy" || expr == "smile") {
        return "happy.gif";
    }
    if (expr == "excited" || expr == "high") {
        return "excited.gif";
    }
    return "blink.gif";
}

std::string NormalizeExpr(const std::string& expr) {
    if (expr == "happy" || expr == "smile") {
        return "happy";
    }
    if (expr == "excited" || expr == "high") {
        return "excited";
    }
    return "blink";
}

}  // namespace

EyePlayer::EyePlayer() = default;

EyePlayer::~EyePlayer() {
    CancelReturnTimer();
    if (return_timer_ != nullptr) {
        esp_timer_delete(return_timer_);
        return_timer_ = nullptr;
    }
}

void EyePlayer::Start() {
    if (started_.exchange(true)) {
        return;
    }

    Settings settings("eyes", false);
    enabled_.store(settings.GetBool("enabled", true));

    esp_timer_create_args_t timer_args = {
        .callback = &EyePlayer::ReturnToBlinkTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "eye_return",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &return_timer_));

    RegisterMcpTools();

    if (enabled_.load()) {
        SetExpr("blink", true);
    } else {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ClearEyes();
        }
    }
    ESP_LOGI(TAG, "Eye player started (enabled=%d)", enabled_.load());
}

void EyePlayer::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddUserOnlyTool(
        "self.eyes.set_enabled",
        "Enable or disable dual-eye animation on the upper screen.",
        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
        [this](const PropertyList& properties) -> ReturnValue {
            SetEnabled(properties["enabled"].value<bool>());
            return true;
        });
    mcp.AddUserOnlyTool(
        "self.eyes.set_expr",
        "Switch eye animation clip. Supported: blink, happy, excited "
        "(aliases: smile->happy, high->excited).",
        PropertyList({Property("expr", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string expr = properties["expr"].value<std::string>();
            bool ok = SetExpr(expr, true);
            return ok;
        });
    mcp.AddUserOnlyTool("self.eyes.get_status",
                        "Get eye animation status and current clip name.", PropertyList(),
                        [this](const PropertyList&) -> ReturnValue {
                            char buf[128];
                            snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"expr\":\"%s\"}",
                                     IsEnabled() ? "true" : "false", CurrentExpr().c_str());
                            return std::string(buf);
                        });
}

void EyePlayer::SetEnabled(bool enabled) {
    enabled_.store(enabled);
    Settings settings("eyes", true);
    settings.SetBool("enabled", enabled);

    auto display = Board::GetInstance().GetDisplay();
    if (!enabled) {
        CancelReturnTimer();
        if (gif_) {
            gif_->Stop();
            gif_.reset();
        }
        asset_image_.reset();
        current_expr_.clear();
        if (display) {
            display->ClearEyes();
            display->ShowNotification("eyes: OFF", 1500);
        }
    } else {
        if (display) {
            display->ShowNotification("eyes: ON", 1500);
        }
        SetExpr("blink", true);
    }
}

std::string EyePlayer::CurrentExpr() const {
    return current_expr_.empty() ? "none" : current_expr_;
}

bool EyePlayer::SetExpr(const std::string& expr, bool force) {
    if (!enabled_.load()) {
        return false;
    }
    std::string name = NormalizeExpr(expr);
    if (!force && name == current_expr_) {
        return true;
    }
    if (!LoadClip(name)) {
        return false;
    }
    current_expr_ = name;

    CancelReturnTimer();
    if (name == "happy" || name == "excited") {
        ScheduleReturnToBlink(kClipDurationMs * kHappyExcitedLoops);
    }
    return true;
}

bool EyePlayer::LoadClip(const std::string& name) {
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGE(TAG, "Assets partition not ready");
        return false;
    }

    const char* asset_name = AssetNameForExpr(name);
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData(asset_name, ptr, size) || ptr == nullptr || size == 0) {
        ESP_LOGE(TAG, "Eye asset missing: %s", asset_name);
        return false;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return false;
    }

    if (gif_) {
        gif_->Stop();
        gif_.reset();
    }
    asset_image_.reset(new LvglRawImage(ptr, size));
    if (!asset_image_->IsGif()) {
        ESP_LOGE(TAG, "Asset is not GIF: %s", asset_name);
        asset_image_.reset();
        return false;
    }

    gif_ = std::make_unique<LvglGif>(asset_image_->image_dsc());
    if (!gif_->IsLoaded()) {
        ESP_LOGE(TAG, "Failed to decode GIF: %s", asset_name);
        gif_.reset();
        asset_image_.reset();
        return false;
    }

    gif_->SetFrameCallback([this]() { OnFrame(); });
    display->SetEyeFrame(static_cast<const void*>(gif_->image_dsc()));
    gif_->Start();
    ESP_LOGI(TAG, "Playing eye clip: %s (%s, %dx%d)", name.c_str(), asset_name, gif_->width(),
             gif_->height());
    return true;
}

void EyePlayer::OnFrame() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || !gif_) {
        return;
    }
    display->SetEyeFrame(static_cast<const void*>(gif_->image_dsc()));
}

void EyePlayer::ScheduleReturnToBlink(uint32_t delay_ms) {
    if (return_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(return_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(return_timer_, (uint64_t)delay_ms * 1000));
}

void EyePlayer::CancelReturnTimer() {
    if (return_timer_ != nullptr) {
        esp_timer_stop(return_timer_);
    }
}

void EyePlayer::ReturnToBlinkTimer(void* arg) {
    auto* self = static_cast<EyePlayer*>(arg);
    if (self == nullptr || !self->enabled_.load()) {
        return;
    }
    self->SetExpr("blink", true);
}

#endif  // CONFIG_XIAOZHI_ENABLE_EYE_ANIM
