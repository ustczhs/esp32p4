#include "backend_link.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "cap_im_wechat.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "settings.h"
#include "system_info.h"
#include "web_socket.h"

#define TAG "BackendLink"

namespace {

constexpr size_t kQueueLen = 8;
constexpr int64_t kUnreachableCooldownUs = 60LL * 1000 * 1000;
constexpr int64_t kPingIntervalUs = 30LL * 1000 * 1000;
constexpr const char* kUnreachableText = "助手暂时不在线，请稍后再试。";

struct Job {
    bool is_reply = false;
    bool announce = true;
    std::string chat_id;
    std::string sender_id;
    std::string message_id;
    std::string text;
};

QueueHandle_t s_queue = nullptr;
std::unique_ptr<WebSocket> s_ws;
std::atomic<bool> s_connected{false};
std::map<std::string, int64_t> s_last_notice_us;

void EnqueueJob(Job* job) {
    if (s_queue == nullptr) {
        delete job;
        return;
    }
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        Job* oldest = nullptr;
        if (xQueueReceive(s_queue, &oldest, 0) == pdTRUE) {
            ESP_LOGW(TAG, "queue full, dropping oldest job");
            delete oldest;
        }
        if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
            delete job;
        }
    }
}

std::string DeriveBackendUrl() {
    Settings ws_settings("websocket", false);
    std::string url = ws_settings.GetString("url");
    if (url.empty()) {
        Settings wifi_settings("wifi", false);
        url = wifi_settings.GetString("ota_url");
        if (url.empty()) {
            url = CONFIG_OTA_URL;
        }
        if (url.compare(0, 7, "http://") == 0) {
            url = "ws://" + url.substr(7);
        } else if (url.compare(0, 8, "https://") == 0) {
            url = "wss://" + url.substr(8);
        }
    }
    auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return "";
    }
    auto path = url.find('/', scheme + 3);
    std::string base = path == std::string::npos ? url : url.substr(0, path);
    return base + "/wechat/v1/";
}

void SendUnreachableNotice(const std::string& chat_id) {
    int64_t now = esp_timer_get_time();
    auto it = s_last_notice_us.find(chat_id);
    if (it != s_last_notice_us.end() && now - it->second < kUnreachableCooldownUs) {
        return;
    }
    s_last_notice_us[chat_id] = now;
    esp_err_t err = cap_im_wechat_send_text(chat_id.c_str(), kUnreachableText);
    ESP_LOGW(TAG, "backend unreachable, notice sent to %s err=%s", chat_id.c_str(),
             esp_err_to_name(err));
}

void HandleReply(const Job* job) {
    esp_err_t err = cap_im_wechat_send_text(job->chat_id.c_str(), job->text.c_str());
    ESP_LOGI(TAG, "wechat reply to %s err=%s: %s", job->chat_id.c_str(), esp_err_to_name(err),
             job->text.c_str());
    if (err == ESP_OK && job->announce) {
        Application::GetInstance().ClawAnnounce(job->text);
    }
}

bool ForwardToBackend(const Job* job) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "message");
    cJSON_AddStringToObject(root, "chat_id", job->chat_id.c_str());
    cJSON_AddStringToObject(root, "sender_id", job->sender_id.c_str());
    cJSON_AddStringToObject(root, "message_id", job->message_id.c_str());
    cJSON_AddStringToObject(root, "text", job->text.c_str());
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(time(nullptr)));
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        return false;
    }
    bool ok = s_ws && s_ws->Send(json);
    cJSON_free(json);
    return ok;
}

void OnWsData(const char* data, size_t len, bool binary) {
    if (binary) {
        return;
    }
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "bad json from backend");
        return;
    }
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "reply") == 0) {
        auto chat_id = cJSON_GetObjectItem(root, "chat_id");
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(chat_id) && cJSON_IsString(text)) {
            auto* job = new Job();
            job->is_reply = true;
            job->chat_id = chat_id->valuestring;
            job->text = text->valuestring;
            auto announce = cJSON_GetObjectItem(root, "announce");
            job->announce = !cJSON_IsBool(announce) || cJSON_IsTrue(announce);
            EnqueueJob(job);
        }
    }
    cJSON_Delete(root);
}

bool EnsureConnected() {
    if (s_ws && s_ws->IsConnected() && s_connected.load()) {
        return true;
    }
    s_connected.store(false);
    s_ws.reset();

    std::string url = DeriveBackendUrl();
    if (url.empty()) {
        ESP_LOGW(TAG, "no backend url yet (OTA not done?)");
        return false;
    }
    Settings ws_settings("websocket", false);
    std::string token = ws_settings.GetString("token");

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    s_ws = network->CreateWebSocket(2);
    if (s_ws == nullptr) {
        return false;
    }
    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) {
            token = "Bearer " + token;
        }
        s_ws->SetHeader("Authorization", token.c_str());
    }
    s_ws->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    s_ws->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    s_ws->SetReceiveBufferSize(8192);
    s_ws->OnData(OnWsData);
    s_ws->OnDisconnected([]() {
        ESP_LOGW(TAG, "backend link disconnected");
        s_connected.store(false);
    });

    ESP_LOGI(TAG, "connecting to %s", url.c_str());
    if (auto result = s_ws->Connect(url.c_str()); !result) {
        ESP_LOGW(TAG, "connect failed: %s", result.error().ToString().c_str());
        s_ws.reset();
        return false;
    }
    s_connected.store(true);
    ESP_LOGI(TAG, "backend link connected");
    return true;
}

void WorkerTask(void* arg) {
    int backoff_ms = 1000;
    int64_t last_ping_us = 0;
    while (true) {
        if (!EnsureConnected()) {
            // Backend down: drain pending inbound messages with an offline notice.
            Job* job = nullptr;
            while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
                if (job->is_reply) {
                    HandleReply(job);
                } else {
                    SendUnreachableNotice(job->chat_id);
                }
                delete job;
            }
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms >= 30000 ? 30000 : backoff_ms * 2;
            continue;
        }
        backoff_ms = 1000;

        Job* job = nullptr;
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (job->is_reply) {
                HandleReply(job);
            } else if (!ForwardToBackend(job)) {
                ESP_LOGW(TAG, "forward failed, backend link down");
                s_connected.store(false);
                SendUnreachableNotice(job->chat_id);
            }
            delete job;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_ping_us >= kPingIntervalUs) {
            last_ping_us = now;
            if (s_ws) {
                s_ws->Ping();
            }
        }
    }
}

esp_err_t ForwardExecute(const char* input_json, const claw_cap_call_context_t* ctx, char* output,
                         size_t output_size) {
    cJSON* root = cJSON_Parse(input_json);
    if (root == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto chat_id = cJSON_GetObjectItem(root, "chat_id");
    auto text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(chat_id) || !cJSON_IsString(text) || !text->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    auto* job = new Job();
    job->chat_id = chat_id->valuestring;
    job->text = text->valuestring;
    auto sender_id = cJSON_GetObjectItem(root, "sender_id");
    if (cJSON_IsString(sender_id)) {
        job->sender_id = sender_id->valuestring;
    }
    auto message_id = cJSON_GetObjectItem(root, "message_id");
    if (cJSON_IsString(message_id)) {
        job->message_id = message_id->valuestring;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "forward chat=%s: %s", job->chat_id.c_str(), job->text.c_str());
    EnqueueJob(job);
    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}

const claw_cap_descriptor_t s_forward_descriptor = {
    .id = "backend_forward",
    .name = "backend_forward",
    .family = "backend",
    .description = "Forward an inbound WeChat text message to the backend agent.",
    .kind = CLAW_CAP_KIND_CALLABLE,
    .cap_flags = 0,
    .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},"
        "\"text\":{\"type\":\"string\"}},\"required\":[\"chat_id\",\"text\"]}",
    .execute = ForwardExecute,
};

// claw_cap_register() keeps a pointer to a stack-local group (vendor bug),
// so register a static group instead.
const claw_cap_group_t s_backend_group = {
    .group_id = "backend_forward",
    .plugin_name = "backend_link",
    .version = "1",
    .descriptors = &s_forward_descriptor,
    .descriptor_count = 1,
};

}  // namespace

esp_err_t BackendLinkRegisterCap() { return claw_cap_register_group(&s_backend_group); }

void BackendLinkStart() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    s_queue = xQueueCreate(kQueueLen, sizeof(Job*));
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "cannot create job queue");
        return;
    }
    claw_task_config_t task_config = {
        .name = "backend_link",
        .stack_size = 12 * 1024,
        .priority = 5,
        .core_id = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    claw_task_create(&task_config, WorkerTask, nullptr, nullptr);
}