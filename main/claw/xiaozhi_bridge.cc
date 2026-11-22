#include "xiaozhi_bridge.h"

#include <cstring>
#include <string>

#include <esp_log.h>

#include "application.h"

#define TAG "ClawBridge"

namespace {

// The official cloud rejects detect texts above ~16 hanzi (16 was rejected in
// practice), so keep the spoken summary well under the limit; the full reply
// goes to the screen instead.
constexpr size_t kSummaryPrefixChars = 5;  // "微信回复："
constexpr size_t kSummaryMaxChars = 12;

std::string Utf8Truncate(const std::string& s, size_t max_chars) {
    size_t chars = 0;
    size_t i = 0;
    while (i < s.size() && chars < max_chars) {
        auto c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        if (i + len > s.size()) {
            break;
        }
        i += len;
        ++chars;
    }
    return s.substr(0, i);
}

}  // namespace

esp_err_t XiaozhiBridgeOutboundResolver(const claw_event_t* event,
                                        const char* target_channel,
                                        const char* target_endpoint,
                                        char* cap_name,
                                        size_t cap_name_size,
                                        void* user_ctx) {
    if (event != nullptr && event->text != nullptr && target_channel != nullptr &&
        strcmp(target_channel, "wechat") == 0) {
        std::string full_text(event->text);
        std::string summary =
            "微信回复：" + Utf8Truncate(full_text, kSummaryMaxChars - kSummaryPrefixChars);
        ESP_LOGI(TAG, "outbound [%s/%s]: %s", target_channel, event->chat_id, event->text);
        Application::GetInstance().ClawAnnounce(summary, full_text);
    }
    // Keep the default binding so the reply is still delivered to WeChat.
    return ESP_ERR_NOT_FOUND;
}