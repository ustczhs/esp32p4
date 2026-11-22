#include "wechat_cli.h"

#include <sdkconfig.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <fcntl.h>
#include <unistd.h>

#include "cap_im_wechat.h"

#define TAG "WechatCli"

namespace {

constexpr const char* kNvsNamespace = "claw";

TaskHandle_t s_watcher_task = nullptr;
bool s_login_force = false;

bool NvsSetString(const char* key, const char* value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed for %s: %s", key, esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool NvsEraseKey(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool NvsHasString(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool ok = nvs_get_str(handle, key, nullptr, &len) == ESP_OK && len > 1;
    nvs_close(handle);
    return ok;
}

void LogStatus(const cap_im_wechat_qr_login_status_t& st) {
    ESP_LOGI(TAG, "login status=%s active=%d configured=%d completed=%d account=%s",
             st.status, st.active, st.configured, st.completed,
             st.account_id[0] ? st.account_id : "-");
    if (st.message[0]) {
        ESP_LOGI(TAG, "%s", st.message);
    }
    if (st.qr_data_url[0]) {
        ESP_LOGI(TAG, "QR_URL: %s", st.qr_data_url);
    }
}

esp_err_t PersistAndApplyLogin(const cap_im_wechat_qr_login_status_t& st) {
    if (!st.token[0] || !st.base_url[0]) {
        ESP_LOGE(TAG, "login completed but token/base_url missing");
        return ESP_ERR_INVALID_STATE;
    }
    if (!NvsSetString("wechat_token", st.token) ||
        !NvsSetString("wechat_base_url", st.base_url)) {
        ESP_LOGE(TAG, "failed to save wechat credentials to NVS");
        return ESP_FAIL;
    }
    if (st.account_id[0] && !NvsSetString("wechat_account_id", st.account_id)) {
        ESP_LOGW(TAG, "account_id not saved (non-fatal)");
    }
    cap_im_wechat_client_config_t config = {
        .token = st.token,
        .base_url = st.base_url,
        .cdn_base_url = nullptr,
        .account_id = st.account_id[0] ? st.account_id : nullptr,
        .app_id = nullptr,
        .client_version = nullptr,
        .route_tag = nullptr,
    };
    esp_err_t err = cap_im_wechat_set_client_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    err = cap_im_wechat_start();
    if (err != ESP_OK) {
        return err;
    }
    cap_im_wechat_qr_login_mark_persisted();
    ESP_LOGI(TAG, "wechat login OK, account=%s; credentials saved, gateway started",
             st.account_id[0] ? st.account_id : "-");
    return ESP_OK;
}

// Runs the QR fetch/poll off the CLI task: the HTTP+cJSON work needs more
// stack than the 4KB eye_cli task has.
void LoginWatcherTask(void* arg) {
    bool force = s_login_force;
    cap_im_wechat_qr_login_status_t st;
    char last_status[32] = "";
    char last_url[256] = "";

    esp_err_t err = cap_im_wechat_qr_login_start(nullptr, force);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qr login start failed: %s", esp_err_to_name(err));
    }

    while (true) {
        if (cap_im_wechat_qr_login_get_status(&st) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (strcmp(st.status, last_status) != 0) {
            ESP_LOGI(TAG, "login: %s - %s", st.status, st.message);
            strlcpy(last_status, st.status, sizeof(last_status));
        }
        if (st.qr_data_url[0] && strcmp(st.qr_data_url, last_url) != 0) {
            ESP_LOGI(TAG, "QR_URL: %s", st.qr_data_url);
            strlcpy(last_url, st.qr_data_url, sizeof(last_url));
        }
        if (st.completed) {
            if (!st.persisted) {
                PersistAndApplyLogin(st);
            }
            break;
        }
        if (!st.active) {
            ESP_LOGW(TAG, "login ended: %s - %s", st.status, st.message);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    s_watcher_task = nullptr;
    vTaskDelete(nullptr);
}

void HandleWechat(const char* args) {
    while (*args == ' ') {
        ++args;
    }
    if (strncmp(args, "login", 5) == 0) {
        if (s_watcher_task != nullptr) {
            ESP_LOGI(TAG, "login already in progress; use: wechat status");
            return;
        }
        s_login_force = strstr(args + 5, "force") != nullptr;
        if (xTaskCreate(LoginWatcherTask, "claw_wx_login", 6 * 1024, nullptr, 3,
                        &s_watcher_task) != pdPASS) {
            s_watcher_task = nullptr;
            ESP_LOGE(TAG, "cannot create login watcher task");
        }
        return;
    }
    if (strcmp(args, "status") == 0) {
        cap_im_wechat_qr_login_status_t st;
        if (cap_im_wechat_qr_login_get_status(&st) == ESP_OK) {
            LogStatus(st);
        } else {
            ESP_LOGE(TAG, "cannot get login status");
        }
        return;
    }
    if (strcmp(args, "cancel") == 0) {
        cap_im_wechat_qr_login_cancel();
        ESP_LOGI(TAG, "login cancelled");
        return;
    }
    if (strcmp(args, "logout") == 0) {
        cap_im_wechat_qr_login_cancel();
        NvsEraseKey("wechat_token");
        NvsEraseKey("wechat_base_url");
        NvsEraseKey("wechat_account_id");
        NvsEraseKey("wechat_cdn_base_url");
        cap_im_wechat_client_config_t config = {
            .token = "",
            .base_url = "",
            .cdn_base_url = nullptr,
            .account_id = nullptr,
            .app_id = nullptr,
            .client_version = nullptr,
            .route_tag = nullptr,
        };
        cap_im_wechat_set_client_config(&config);
        ESP_LOGI(TAG, "wechat logged out; credentials erased");
        return;
    }
    ESP_LOGI(TAG, "usage: wechat login [force] | status | cancel | logout");
}

void HandleLlm(const char* args) {
    while (*args == ' ') {
        ++args;
    }
    const char* value = strchr(args, ' ');
    char key[16] = "";
    if (value != nullptr) {
        size_t len = static_cast<size_t>(value - args);
        if (len >= sizeof(key)) {
            len = sizeof(key) - 1;
        }
        memcpy(key, args, len);
        key[len] = '\0';
        ++value;
        while (*value == ' ') {
            ++value;
        }
    } else {
        strlcpy(key, args, sizeof(key));
    }

    if (strcmp(key, "status") == 0) {
        ESP_LOGI(TAG, "llm: key=%s (model/base_url: see boot log; reboot applies changes)",
                 NvsHasString("llm_api_key") ? "configured" : "missing");
        return;
    }
    const char* nvs_key = nullptr;
    if (strcmp(key, "key") == 0) {
        nvs_key = "llm_api_key";
    } else if (strcmp(key, "model") == 0) {
        nvs_key = "llm_model";
    } else if (strcmp(key, "url") == 0) {
        nvs_key = "llm_base_url";
    }
    if (nvs_key == nullptr || value == nullptr || !value[0]) {
        ESP_LOGI(TAG, "usage: llm key <api_key> | model <name> | url <base_url> | status");
        return;
    }
    if (NvsSetString(nvs_key, value)) {
        ESP_LOGI(TAG, "llm %s saved to NVS; reboot to apply", key);
    } else {
        ESP_LOGE(TAG, "failed to save llm %s", key);
    }
}

#if !CONFIG_XIAOZHI_ENABLE_EYE_ANIM
void StdinCliTask(void* arg) {
    char line[160];
    size_t pos = 0;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    ESP_LOGI(TAG, "claw serial CLI ready. Type: wechat login");
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
            WechatCliHandleLine(line);
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
#endif

}  // namespace

bool WechatCliHandleLine(const char* line) {
    while (*line == ' ') {
        ++line;
    }
    if (strncmp(line, "wechat", 6) == 0 && (line[6] == '\0' || line[6] == ' ')) {
        HandleWechat(line + 6);
        return true;
    }
    if (strncmp(line, "llm", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        HandleLlm(line + 3);
        return true;
    }
    return false;
}

void WechatCliStart() {
#if !CONFIG_XIAOZHI_ENABLE_EYE_ANIM
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    xTaskCreate(StdinCliTask, "claw_cli", 4096, nullptr, 3, nullptr);
#endif
}