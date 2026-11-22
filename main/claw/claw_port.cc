#include "claw_port.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sdkconfig.h>

#include "cap_im_wechat.h"
#include "cap_system.h"
#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_event_router.h"
#include "claw_paths.h"
#include "wechat_cli.h"
#include "xiaozhi_bridge.h"

#define TAG "ClawPort"

namespace {

constexpr const char* kNvsNamespace = "claw";
constexpr const char* kDataRoot = "/claw";
constexpr const char* kRulesPath = "/claw/router_rules/router_rules.json";

constexpr const char* kSeedRules = R"JSON([
  {
    "id": "im_any_message_agent",
    "description": "Route IM text messages to the agent.",
    "enabled": true,
    "consume_on_match": true,
    "match": {
      "event_type": "message",
      "event_key": "text",
      "content_type": "text"
    },
    "actions": [
      {
        "type": "run_agent",
        "input": {
          "target_channel": "{{event.source_channel}}",
          "session_policy": "chat"
        }
      }
    ]
  },
  {
    "id": "agent_out_message_send_message",
    "description": "Deliver agent output messages to the original IM chat.",
    "enabled": true,
    "consume_on_match": true,
    "match": {
      "source_cap": "claw_core",
      "event_type": "out_message",
      "content_type": "text"
    },
    "actions": [
      {
        "type": "send_message",
        "input": {
          "channel": "{{event.source_channel}}",
          "chat_id": "{{event.chat_id}}",
          "message": "{{event.text}}"
        }
      }
    ]
  }
])JSON";

constexpr const char* kSystemPrompt =
    "你是运行在 ESP32 设备上的微信助手。用简体中文回复微信消息，"
    "简短口语化，一般不超过三句话，不要使用 Markdown 格式。";

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
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(kDataRoot, "claw_data", &mount_config,
                                                     &wl_handle);
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
        fclose(f);
        return ESP_OK;
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
        .enable_inbound_attachments = true,
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

esp_err_t StartAgent() {
    std::string api_key, model, base_url;
    NvsGetString("llm_api_key", api_key);
    if (!NvsGetString("llm_model", model)) {
        model = CONFIG_CLAW_LLM_MODEL;
    }
    if (!NvsGetString("llm_base_url", base_url)) {
        base_url = CONFIG_CLAW_LLM_BASE_URL;
    }
    if (api_key.empty()) {
        ESP_LOGW(TAG, "no LLM API key in NVS (claw/llm_api_key); agent replies will fail");
    }

    claw_core_config_t core_config = {};
    core_config.api_key = api_key.c_str();
    core_config.backend_type = CONFIG_CLAW_LLM_BACKEND;
    core_config.model = model.c_str();
    core_config.base_url = base_url.c_str();
    core_config.timeout_ms = 60000;
    core_config.max_tokens = 1024;
    core_config.supports_tools = true;
    core_config.supports_vision = false;
    core_config.system_prompt = kSystemPrompt;
    core_config.call_cap = claw_cap_call_from_core;
    core_config.task_stack_size = 16 * 1024;
    core_config.task_priority = 5;
    core_config.task_core = tskNO_AFFINITY;
    core_config.max_tool_iterations = 6;
    core_config.request_queue_len = 4;
    core_config.response_queue_len = 4;
    core_config.max_context_providers = 4;

    ESP_LOGI(TAG, "root agent backend=%s model=%s base_url=%s key=%s",
             CONFIG_CLAW_LLM_BACKEND, model.c_str(), base_url.c_str(),
             api_key.empty() ? "missing" : "configured");
    claw_agent_mgr_config_t mgr_config = {
        .core_config = &core_config,
        .base_context_providers = nullptr,
        .base_context_provider_count = 0,
        .root_agent_system_prompt = nullptr,
        .subagent_system_prompt = nullptr,
        .subagent_type_prompts = nullptr,
        .subagent_type_prompt_count = 0,
    };
    esp_err_t err = claw_agent_mgr_init(&mgr_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "agent mgr init failed: %s", esp_err_to_name(err));
        return err;
    }
    const char* root_agent_id = nullptr;
    err = claw_agent_mgr_create_root_agent(&root_agent_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "root agent create failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "root agent ready id=%s", root_agent_id ? root_agent_id : "?");
    return ESP_OK;
}

bool NetworkReady(void* ctx) {
    return true;
}

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
        router_config.default_route_messages_to_agent = true;
        router_config.outbound_resolver = XiaozhiBridgeOutboundResolver;
        err = claw_event_router_init(&router_config);
    }
    if (err == ESP_OK) {
        step = "cap_init";
        err = claw_cap_init();
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
        step = "outbound_binding";
        err = claw_event_router_register_outbound_binding("wechat", "wechat_send_message");
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
        step = "agent_start";
        err = StartAgent();
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