#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_EYE_ANIM

#include "eye_player.h"

#include "application.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "gif/lvgl_gif.h"
#include "lvgl_image.h"
#include "mcp_server.h"
#include "settings.h"

#if CONFIG_CLAW_ENABLE
#include "claw/wechat_cli.h"
#endif

#include <fcntl.h>
#include <unistd.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#define TAG "EyePlayer"

namespace {

const char* kExprOrder[] = {
    "blink",
    "happy",
    "excited",
    "cute",
    "wink",
    "listening",
    "thinking",
    "look_around",
    "shake_head",
    "sad",
    "angry",
    "fear",
    "shy",
    "smirk",
    "startled",
    "sleepy",
    "wake_up",
    "coquettish",
    "confused",
    "anxious",
    "focused",
    "called",
    "blink_then_happy",
};

const char* kExprLabelZh[] = {
    "常规眨眼",
    "开心",
    "兴奋",
    "可爱",
    "抛媚眼",
    "聆听",
    "思考",
    "四处张望",
    "摇头",
    "伤心",
    "生气",
    "害怕",
    "害篡",
    "坏笑",
    "惊吓",
    "困了",
    "弄醒",
    "撒娇",
    "疑惑",
    "着急",
    "专注",
    "被呼叫",
    "先眨眼再开心",
};

constexpr size_t kExprCount = sizeof(kExprOrder) / sizeof(kExprOrder[0]);
static_assert(sizeof(kExprLabelZh) / sizeof(kExprLabelZh[0]) == kExprCount, "label count");

bool IsKnownExpr(const std::string& name) {
    for (size_t i = 0; i < kExprCount; ++i) {
        if (name == kExprOrder[i]) {
            return true;
        }
    }
    return false;
}

std::string AssetNameForExpr(const std::string& expr) {
    return expr + ".gif";
}

uint32_t DurationMsForExpr(const std::string& name) {
    if (name == "thinking") {
        return 6000;
    }
    if (name == "wink" || name == "called") {
        return 3400;
    }
    if (name == "focused" || name == "shy" || name == "startled" || name == "coquettish" ||
        name == "confused") {
        return 3100;
    }
    return 4100;
}

std::string NormalizeExpr(const std::string& expr) {
    if (IsKnownExpr(expr)) {
        return expr;
    }
    if (expr == "smile") {
        return "happy";
    }
    if (expr == "high") {
        return "excited";
    }
    if (expr == "idle" || expr == "neutral") {
        return "blink";
    }
    if (expr == "fearful" || expr == "scared") {
        return "fear";
    }
    if (expr == "开心" || expr == "笑") {
        return "happy";
    }
    if (expr == "兴奋" || expr == "激动") {
        return "excited";
    }
    if (expr == "眨眼" || expr == "待机") {
        return "blink";
    }
    if (expr == "可爱") {
        return "cute";
    }
    if (expr == "抛媚眼") {
        return "wink";
    }
    if (expr == "聆听") {
        return "listening";
    }
    if (expr == "思考") {
        return "thinking";
    }
    if (expr == "四处张望") {
        return "look_around";
    }
    if (expr == "摇头") {
        return "shake_head";
    }
    if (expr == "伤心") {
        return "sad";
    }
    if (expr == "生气") {
        return "angry";
    }
    if (expr == "害怕") {
        return "fear";
    }
    if (expr == "害篡") {
        return "shy";
    }
    if (expr == "坏笑") {
        return "smirk";
    }
    if (expr == "惊吓") {
        return "startled";
    }
    if (expr == "困了" || expr == "困") {
        return "sleepy";
    }
    if (expr == "弄醒") {
        return "wake_up";
    }
    if (expr == "撒娇") {
        return "coquettish";
    }
    if (expr == "疑惑") {
        return "confused";
    }
    if (expr == "着急") {
        return "anxious";
    }
    if (expr == "专注") {
        return "focused";
    }
    if (expr == "被呼叫" || expr == "呼叫") {
        return "called";
    }
    return "blink";
}

std::string ExprFromEmotion(const std::string& emotion) {
    if (emotion == "happy" || emotion == "laughing" || emotion == "funny" ||
        emotion == "delicious" || emotion == "relaxed") {
        return "happy";
    }
    if (emotion == "loving" || emotion == "kissy") {
        return "coquettish";
    }
    if (emotion == "winking") {
        return "wink";
    }
    if (emotion == "cool" || emotion == "confident") {
        return "focused";
    }
    if (emotion == "surprised" || emotion == "shocked") {
        return "startled";
    }
    if (emotion == "angry") {
        return "angry";
    }
    if (emotion == "embarrassed") {
        return "shy";
    }
    if (emotion == "silly") {
        return "cute";
    }
    if (emotion == "sad" || emotion == "crying") {
        return "sad";
    }
    if (emotion == "sleepy") {
        return "sleepy";
    }
    if (emotion == "thinking") {
        return "thinking";
    }
    if (emotion == "confused") {
        return "confused";
    }
    return "blink";
}

void TrimInPlace(char* s) {
    if (s == nullptr) {
        return;
    }
    // Keep only ASCII printable (USB-UART often injects noise bytes).
    char* w = s;
    for (char* r = s; *r; ++r) {
        unsigned char c = static_cast<unsigned char>(*r);
        if (c >= 0x20 && c < 0x7f) {
            *w++ = static_cast<char>(c);
        }
    }
    *w = '\0';
    // trim spaces
    char* start = s;
    while (*start && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && std::isspace(static_cast<unsigned char>(s[n - 1]))) {
        s[--n] = '\0';
    }
}

}  // namespace

EyePlayer::EyePlayer() = default;

EyePlayer::~EyePlayer() {
    CancelReturnTimer();
    if (return_timer_ != nullptr) {
        esp_timer_delete(return_timer_);
        return_timer_ = nullptr;
    }
    if (cli_task_ != nullptr) {
        vTaskDelete(cli_task_);
        cli_task_ = nullptr;
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
    StartSerialCli();

    if (enabled_.load()) {
        SetExpr("blink", true, false);
    } else {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ClearEyes();
        }
    }
    ESP_LOGI(TAG, "Eye player started (enabled=%d, clips=%u)", enabled_.load(),
             (unsigned)kExprCount);
    ESP_LOGI(TAG, "Serial UART: type 'eyes' then 0-%u to switch expression",
             (unsigned)(kExprCount - 1));
}

void EyePlayer::StartSerialCli() {
    if (cli_task_ != nullptr) {
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(&EyePlayer::SerialCliTask, "eye_cli", 4096, this, 3,
                                            &cli_task_, 0);
    if (ok != pdPASS) {
        cli_task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create eye serial CLI task");
    }
}

bool HandleOtaCli(const char* line) {
    if (strncmp(line, "ota", 3) != 0 || (line[3] != '\0' && line[3] != ' ')) {
        return false;
    }
    const char* args = line + 3;
    while (*args == ' ') {
        ++args;
    }
    Settings settings("wifi", true);
    if (strncmp(args, "url", 3) == 0 && (args[3] == '\0' || args[3] == ' ')) {
        const char* value = args + 3;
        while (*value == ' ') {
            ++value;
        }
        if (*value == '\0') {
            std::string url = settings.GetString("ota_url");
            ESP_LOGI(TAG, "ota_url=%s (empty = built-in default)", url.c_str());
        } else if (strcmp(value, "clear") == 0) {
            // EraseKey does not commit; an empty value also falls back to CONFIG_OTA_URL.
            settings.SetString("ota_url", "");
            ESP_LOGI(TAG, "ota_url cleared; reboot to use built-in default");
        } else {
            settings.SetString("ota_url", value);
            ESP_LOGI(TAG, "ota_url=%s saved; reboot to apply", value);
        }
    } else {
        ESP_LOGI(TAG, "usage: ota url <http_url> | ota url clear | ota url");
    }
    return true;
}

void CliPrint(const char* s) {
    if (s == nullptr || s[0] == '\0') {
        return;
    }
    // Primary console is UART0 -> host /dev/ttyACM0 (CH343).
    fputs(s, stdout);
    fflush(stdout);
}

void EyePlayer::PrintExprList() const {
    char line[96];
    CliPrint("\r\n======== Eye expressions (0-22) ========\r\n");
    for (size_t i = 0; i < kExprCount; ++i) {
        const char mark = (current_expr_ == kExprOrder[i]) ? '*' : ' ';
        snprintf(line, sizeof(line), " %c %2u  %-18s  %s\r\n", mark, (unsigned)i, kExprOrder[i],
                 kExprLabelZh[i]);
        CliPrint(line);
    }
    CliPrint("---------------------------------------\r\n");
    snprintf(line, sizeof(line), "Commands: <0-%u> switch | list | q exit\r\n",
             (unsigned)(kExprCount - 1));
    CliPrint(line);
    CliPrint("=======================================\r\n");
}

size_t EyePlayer::ExprCount() const {
    return kExprCount;
}

const char* EyePlayer::ExprName(size_t index) const {
    if (index >= kExprCount) {
        return "";
    }
    return kExprOrder[index];
}

const char* EyePlayer::ExprLabelZh(size_t index) const {
    if (index >= kExprCount) {
        return "";
    }
    return kExprLabelZh[index];
}

void EyePlayer::HandleSerialLine(const char* raw, bool& interactive) {
    char line[80];
    snprintf(line, sizeof(line), "%s", raw ? raw : "");
    TrimInPlace(line);
    if (line[0] == '\0') {
        return;
    }

    auto do_switch = [this](int idx) {
        Application::GetInstance().Schedule([this, idx]() {
            if (!SetExprByIndex(idx, false)) {
                return;
            }
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                char buf[48];
                snprintf(buf, sizeof(buf), "eyes: %d %s", idx, CurrentExpr().c_str());
                display->ShowNotification(buf, 1500);
            }
        });
        char msg[80];
        snprintf(msg, sizeof(msg), "OK %d -> %s (%s)\r\n", idx, kExprOrder[idx], kExprLabelZh[idx]);
        CliPrint(msg);
        ESP_LOGI(TAG, "%s", msg);
    };

    if (strcmp(line, "eyes") == 0 || strcmp(line, "eye") == 0) {
        interactive = true;
        PrintExprList();
        CliPrint("eyes> ");
        return;
    }

    if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
        interactive = false;
        Application::GetInstance().Schedule([this]() { SetExpr("blink", true, false); });
        CliPrint("exit eye mode -> blink\r\n");
        ESP_LOGI(TAG, "exit eye mode");
        return;
    }

    if (strcmp(line, "list") == 0 || strcmp(line, "l") == 0 || strcmp(line, "?") == 0) {
        interactive = true;
        PrintExprList();
        CliPrint("eyes> ");
        return;
    }

    // "eye 3" / "eyes 3"
    int idx = -1;
    if (sscanf(line, "eye %d", &idx) == 1 || sscanf(line, "eyes %d", &idx) == 1) {
        if (idx >= 0 && idx < (int)kExprCount) {
            interactive = true;
            do_switch(idx);
            if (interactive) {
                CliPrint("eyes> ");
            }
        } else {
            CliPrint("ERR index out of range\r\n");
        }
        return;
    }

    // Bare number: always accepted (enter interactive if needed).
    char* endptr = nullptr;
    long v = strtol(line, &endptr, 10);
    if (endptr != line && *endptr == '\0' && v >= 0 && v < (long)kExprCount) {
        interactive = true;
        do_switch(static_cast<int>(v));
        CliPrint("eyes> ");
        return;
    }

    if (interactive) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Unknown '%s'. Try number / list / q\r\n", line);
        CliPrint(msg);
        CliPrint("eyes> ");
    }
}

void EyePlayer::SerialCliTask(void* arg) {
    auto* self = static_cast<EyePlayer*>(arg);
    char line[80];
    size_t pos = 0;
    bool interactive = false;

    // Host /dev/ttyACM0 is CH343 USB-UART -> ESP console UART.
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    ESP_LOGI(TAG, "Eye serial CLI ready (console UART). Type: eyes");

    while (true) {
        int c = fgetc(stdin);
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[pos] = '\0';
            pos = 0;
            ESP_LOGI(TAG, "serial cmd: [%s]", line);
            if (HandleOtaCli(line)) {
                continue;
            }
#if CONFIG_CLAW_ENABLE
            if (WechatCliHandleLine(line)) {
                continue;
            }
#endif
            self->HandleSerialLine(line, interactive);
            continue;
        }
        if (c == 0x08 || c == 0x7f) {
            if (pos > 0) {
                --pos;
            }
            continue;
        }
        if (pos + 1 < sizeof(line)) {
            line[pos++] = static_cast<char>(c);
        }
    }
}

void EyePlayer::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.eyes.set_expr",
        "Switch the dual-eye animation (pre-stitched 400x200 GIF). "
        "Arg expr: blink, happy, excited, cute, wink, listening, thinking, "
        "look_around, shake_head, sad, angry, fear, shy, smirk, startled, "
        "sleepy, wake_up, coquettish, confused, anxious, focused, called, "
        "blink_then_happy. Also accepts Chinese aliases. "
        "Call when user asks to change eye expression.",
        PropertyList({Property("expr", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string expr = properties["expr"].value<std::string>();
            // Explicit MCP request: hold (no auto-return).
            bool ok = SetExpr(expr, true, false);
            if (!ok) {
                return false;
            }
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ShowNotification(std::string("eyes: ") + CurrentExpr(), 1200);
            }
            return CurrentExpr();
        });

    mcp.AddTool(
        "self.eyes.next",
        "Cycle to the next packaged eye expression and hold it. "
        "Call when user asks to switch eye expression.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue { return CycleNext(); });

    mcp.AddUserOnlyTool(
        "self.eyes.set_enabled",
        "Enable or disable dual-eye animation on the upper screen.",
        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
        [this](const PropertyList& properties) -> ReturnValue {
            SetEnabled(properties["enabled"].value<bool>());
            return true;
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
        if (display) {
            DisplayLockGuard lock(display);
            if (lock) {
                display->DetachEyeImageSrc();
                if (gif_) {
                    gif_->Stop();
                    gif_.reset();
                }
                asset_image_.reset();
            }
            current_expr_.clear();
            display->ClearEyes();
            display->ShowNotification("eyes: OFF", 1500);
        } else {
            if (gif_) {
                gif_->Stop();
                gif_.reset();
            }
            asset_image_.reset();
            current_expr_.clear();
        }
    } else {
        if (display) {
            display->ShowNotification("eyes: ON", 1500);
        }
        SetExpr("blink", true, false);
    }
}

void EyePlayer::Pause() {
    if (!started_.load() || paused_.exchange(true)) {
        return;
    }
    CancelReturnTimer();
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        DisplayLockGuard lock(display);
        if (lock) {
            display->DetachEyeImageSrc();
            if (gif_) {
                gif_->Stop();
                gif_.reset();
            }
            asset_image_.reset();
        }
        current_expr_.clear();
        display->ClearEyes();
    } else {
        if (gif_) {
            gif_->Stop();
            gif_.reset();
        }
        asset_image_.reset();
        current_expr_.clear();
    }
    ESP_LOGI(TAG, "Eye player paused");
}

void EyePlayer::Resume() {
    if (!paused_.exchange(false)) {
        return;
    }
    ESP_LOGI(TAG, "Eye player resumed");
    if (enabled_.load()) {
        SetExpr("blink", true, false);
    }
}

std::string EyePlayer::CurrentExpr() const {
    return current_expr_.empty() ? "none" : current_expr_;
}

bool EyePlayer::SetExpr(const std::string& expr, bool force, bool auto_return) {
    if (!enabled_.load() || paused_.load()) {
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
    // Emotion-driven clips may auto-return; manual/BOOT/serial hold.
    if (auto_return && name != "blink") {
        ScheduleReturnToBlink(DurationMsForExpr(name));
    }
    return true;
}

bool EyePlayer::SetExprByIndex(int index, bool auto_return) {
    if (index < 0 || index >= (int)kExprCount) {
        return false;
    }
    return SetExpr(kExprOrder[index], true, auto_return);
}

bool EyePlayer::SetExprFromEmotion(const std::string& emotion) {
    if (!started_.load() || !enabled_.load()) {
        return false;
    }
    // Cloud emotion is transient -> auto-return to blink.
    return SetExpr(ExprFromEmotion(emotion), false, true);
}

std::string EyePlayer::CycleNext() {
    if (!enabled_.load()) {
        SetEnabled(true);
    }
    size_t idx = 0;
    for (size_t i = 0; i < kExprCount; ++i) {
        if (current_expr_ == kExprOrder[i]) {
            idx = (i + 1) % kExprCount;
            break;
        }
    }
    std::string next = kExprOrder[idx];
    // BOOT cycle: hold, do not auto-return.
    SetExpr(next, true, false);
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        char buf[48];
        snprintf(buf, sizeof(buf), "eyes: %u %s", (unsigned)idx, next.c_str());
        display->ShowNotification(buf, 1500);
    }
    ESP_LOGI(TAG, "CycleNext -> [%u] %s", (unsigned)idx, next.c_str());
    return next;
}

void EyePlayer::RequestCycleNext() {
    Application::GetInstance().Schedule([]() { EyePlayer::GetInstance().CycleNext(); });
}

bool EyePlayer::LoadClip(const std::string& name) {
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGE(TAG, "Assets partition not ready");
        return false;
    }

    std::string asset_name = AssetNameForExpr(name);
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData(asset_name, ptr, size) || ptr == nullptr || size == 0) {
        ESP_LOGE(TAG, "Eye asset missing: %s", asset_name.c_str());
        return false;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return false;
    }

    DisplayLockGuard lock(display);
    if (!lock) {
        return false;
    }

    display->DetachEyeImageSrc();
    if (gif_) {
        gif_->Stop();
        gif_.reset();
    }
    asset_image_.reset(new LvglRawImage(ptr, size));
    if (!asset_image_->IsGif()) {
        ESP_LOGE(TAG, "Asset is not GIF: %s", asset_name.c_str());
        asset_image_.reset();
        return false;
    }

    gif_ = std::make_unique<LvglGif>(asset_image_->image_dsc());
    if (!gif_->IsLoaded()) {
        ESP_LOGE(TAG, "Failed to decode GIF: %s", asset_name.c_str());
        gif_.reset();
        asset_image_.reset();
        return false;
    }

    // Loop while held (manual / serial). Emotion auto-return still uses timer.
    gif_->SetLoopCount(0);
    gif_->SetFrameCallback([this]() { OnFrame(); });
    display->SetEyeFrame(static_cast<const void*>(gif_->image_dsc()));
    gif_->Start();

    ESP_LOGI(TAG, "Playing eye clip: %s (%s, %dx%d) free_sram=%u free_psram=%u", name.c_str(),
             asset_name.c_str(), gif_->width(), gif_->height(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
    Application::GetInstance().Schedule([self]() {
        if (self->enabled_.load()) {
            self->SetExpr("blink", true, false);
        }
    });
}

#endif  // CONFIG_XIAOZHI_ENABLE_EYE_ANIM
