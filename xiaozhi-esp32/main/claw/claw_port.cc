#include "claw_port.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sdkconfig.h>

#include "backend_link.h"
#include "cap_im_wechat.h"
#include "cap_system.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "claw_paths.h"
#include "wechat_cli.h"

#define TAG "ClawPort"

namespace {

constexpr const char* kNvsNamespace = "claw";
constexpr const char* kDataRoot = "/claw";
constexpr const char* kRulesPath = "/claw/router_rules/router_rules.json";

// v2: agent processing lives in the backend; the router only forwards inbound
// text to the backend link cap. The marker detects v1 rule files on upgrade.
constexpr const char* kRulesMarker = "im_text_to_backend";

constexpr const char* kSeedRules = R"JSON([
  {
    "id": "im_text_to_backend",
    "description": "Forward IM text messages to the backend agent link.",
    "enabled": true,
    "consume_on_match": true,
    "match": {
      "event_type": "message",
      "event_key": "text",
      "content_type": "text"
    },
    "actions": [
      {
        "type": "call_cap",
        "cap": "backend_forward",
        "input": {
          "chat_id": "{{event.chat_id}}",
          "sender_id": "{{event.sender_id}}",
          "message_id": "{{event.message_id}}",
          "text": "{{event.text}}"
        }
      }
    ]
  }
])JSON";

bool NvsGetString(const char* key, std::string& out) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool ok = false;
    if (nvs_get_str(handle, key, nullptr, &len) == ESP_OK && len > 1) {
        out.resize(len - 1);
        ok = nvs_get_str(handle, key, out.data(), &len) == ESP_OK;
        if (!ok) {
            out.clear();
        }
    }
    nvs_close(handle);
    return ok;
}

esp_err_t MountDataPartition() {
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t err =
        esp_vfs_fat_spiflash_mount_rw_wl(kDataRoot, "claw_data", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "FATFS mounted at %s", kDataRoot);
    return ESP_OK;
}

esp_err_t EnsureSeedRules() {
    FILE* f = fopen(kRulesPath, "r");
    if (f != nullptr) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        if (strstr(buf, kRulesMarker) != nullptr) {
            return ESP_OK;
        }
        ESP_LOGI(TAG, "old router rules found, migrating to v2");
    }
    mkdir("/claw/router_rules", 0755);
    f = fopen(kRulesPath, "w");
    if (f == nullptr) {
        ESP_LOGE(TAG, "cannot create %s", kRulesPath);
        return ESP_FAIL;
    }
    fputs(kSeedRules, f);
    fclose(f);
    ESP_LOGI(TAG, "seed router rules written");
    return ESP_OK;
}

esp_err_t SetupWechat() {
    char inbox_path[64];
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, "inbox", inbox_path, sizeof(inbox_path));
    if (err != ESP_OK) {
        return err;
    }
    mkdir(inbox_path, 0755);
    cap_im_wechat_attachment_config_t attachment_config = {
        .storage_root_dir = inbox_path,
        .max_inbound_file_bytes = 512 * 1024,
        .enable_inbound_attachments = false,
    };
    err = cap_im_wechat_set_attachment_config(&attachment_config);
    if (err != ESP_OK) {
        return err;
    }

    std::string token, base_url, account_id, cdn_base_url;
    if (NvsGetString("wechat_token", token) && NvsGetString("wechat_base_url", base_url)) {
        NvsGetString("wechat_account_id", account_id);
        NvsGetString("wechat_cdn_base_url", cdn_base_url);
        cap_im_wechat_client_config_t client_config = {
            .token = token.c_str(),
            .base_url = base_url.c_str(),
            .cdn_base_url = cdn_base_url.empty() ? nullptr : cdn_base_url.c_str(),
            .account_id = account_id.empty() ? nullptr : account_id.c_str(),
            .app_id = nullptr,
            .client_version = nullptr,
            .route_tag = nullptr,
        };
        err = cap_im_wechat_set_client_config(&client_config);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "wechat credentials restored from NVS");
    } else {
        ESP_LOGI(TAG, "wechat not logged in yet (use: wechat login)");
    }
    return ESP_OK;
}

bool NetworkReady(void* ctx) { return true; }

void AssemblyTask(void* arg) {
    const char* step = "mount";
    esp_err_t err = MountDataPartition();
    if (err == ESP_OK) {
        step = "paths_data";
        err = claw_paths_set(CLAW_PATH_DATA, kDataRoot);
    }
    if (err == ESP_OK) {
        step = "paths_system";
        mkdir("/claw/system", 0755);
        err = claw_paths_set(CLAW_PATH_SYSTEM, "/claw/system");
    }
    if (err == ESP_OK) {
        step = "seed_rules";
        err = EnsureSeedRules();
    }

    if (err == ESP_OK) {
        step = "router_init";
        claw_event_router_config_t router_config = {};
        router_config.rules_path = kRulesPath;
        router_config.task_stack_size = 8 * 1024;
        router_config.task_priority = 5;
        router_config.task_core = tskNO_AFFINITY;
        router_config.agent_submit_timeout_ms = 1000;
        router_config.default_route_messages_to_agent = false;
        err = claw_event_router_init(&router_config);
    }
    if (err == ESP_OK) {
        step = "cap_init";
        err = claw_cap_init();
    }
    if (err == ESP_OK) {
        step = "backend_cap_register";
        err = BackendLinkRegisterCap();
    }
    if (err == ESP_OK) {
        step = "wechat_register_group";
        err = cap_im_wechat_register_group();
    }
    if (err == ESP_OK) {
        step = "system_register_group";
        err = cap_system_register_group();
    }
    if (err == ESP_OK) {
        step = "wechat_setup";
        err = SetupWechat();
    }
    if (err == ESP_OK) {
        step = "cap_start_all";
        err = claw_cap_start_all();
    }
    if (err == ESP_OK) {
        step = "backend_link_start";
        BackendLinkStart();
    }
    if (err == ESP_OK) {
        step = "router_start";
        err = claw_event_router_start();
    }
    if (err == ESP_OK) {
        step = "time_sync";
        cap_system_time_sync_service_config_t sync_config = {
            .network_ready = NetworkReady,
            .network_ready_ctx = nullptr,
            .on_sync_success = nullptr,
            .on_sync_success_ctx = nullptr,
            .disconnected_retry_ms = 0,
            .sync_retry_ms = 0,
        };
        err = cap_system_time_sync_service_start(&sync_config);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "esp-claw subsystem started");
    } else {
        ESP_LOGE(TAG, "esp-claw subsystem failed at %s: %s", step, esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

}  // namespace

void ClawPortStart() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    WechatCliStart();
    xTaskCreate(AssemblyTask, "claw_port", 8 * 1024, nullptr, 3, nullptr);
}